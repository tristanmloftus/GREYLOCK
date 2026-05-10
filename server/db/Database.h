#pragma once

// Phase 4.E: SQLCipher replaces plain SQLite.
// The header is installed by Homebrew at include/sqlcipher/sqlite3.h.
// We include it via the <sqlite3.h> alias because SQLCIPHER_ROOT is
// set as an include directory in CMakeLists.txt, making the sqlcipher/
// subdirectory the include root.  Any translation unit that includes
// this header automatically gets the SQLCipher build of sqlite3.h
// (not the plain SQLite one) because sqlcipher appears first in the
// include path.
#include <sqlite3.h>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Statement — RAII wrapper around sqlite3_stmt*.
//
// Created by Database::prepare().  The caller steps/resets/finalizes via the
// raw sqlite3_stmt* returned by get(), or by calling step() / reset().
// Destructor finalizes the statement.  Move-only.
// ---------------------------------------------------------------------------
class Statement {
public:
    explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
    ~Statement() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }

    // Move-only.
    Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
        other.stmt_ = nullptr;
    }
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (stmt_) sqlite3_finalize(stmt_);
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Access the raw sqlite3_stmt* for sqlite3_bind_* / sqlite3_column_* calls.
    sqlite3_stmt* get() const noexcept { return stmt_; }

    // Calls sqlite3_step().  Returns SQLITE_ROW, SQLITE_DONE, or another
    // SQLite result code on error.
    int step() { return sqlite3_step(stmt_); }

    // Calls sqlite3_reset() so the statement can be re-executed.
    void reset() { sqlite3_reset(stmt_); }

private:
    sqlite3_stmt* stmt_{nullptr};
};

// ---------------------------------------------------------------------------
// Database — RAII wrapper around sqlite3* (backed by SQLCipher).
//
// Constructor opens (or creates) the database file at `path`.  Use ":memory:"
// for an in-memory database (useful for tests and schema bootstrapping).
// Destructor closes the database.  Move-only.
//
// master_key_hex (optional):
//   If present, must be exactly 64 lowercase hex characters (32 bytes).
//   After sqlite3_open, `PRAGMA key = "x'<hex>'"` is executed to decrypt /
//   encrypt the database.  A sanity query (`SELECT count(*) FROM sqlite_master`)
//   is then run to verify the key is correct — if it fails the constructor
//   throws "Database: master key mismatch for '<path>'".
//
//   If absent (std::nullopt), the database is opened without encryption.
//   For :memory: databases in tests this is fine.  For file databases on disk
//   this means the file is NOT encrypted.  Server/main.cpp enforces that a
//   key is supplied when opening an existing file (see open_db_with_migrations).
//
// Guardrail: master_key_hex is never logged, stored, or echoed.
//
// Exceptions: constructor throws std::runtime_error on open or key failure.
//             exec() throws std::runtime_error if the SQL fails.
//             prepare() throws std::runtime_error if the SQL cannot be parsed.
//
// Thread safety: same as SQLite — the Database object is NOT thread-safe.
// Each thread should hold its own Database instance (or serialize access).
// ---------------------------------------------------------------------------
class Database {
public:
    // Open `path` without encryption.  Suitable for :memory: databases and
    // unencrypted file databases (dev/test without TF_MASTER_KEY).
    explicit Database(std::string_view path)
        : Database(path, std::nullopt) {}

    // Open `path` with an optional master key.
    //
    // master_key_hex: exactly 64 hex chars (32 bytes).  If std::nullopt, no
    // PRAGMA key is applied.  If present, it must be 64 lowercase hex chars;
    // any other length or non-hex content throws std::runtime_error.
    //
    // GUARDRAIL: do NOT log master_key_hex.  The string is never stored
    // beyond this constructor scope.
    Database(std::string_view path, std::optional<std::string> master_key_hex) {
        // Validate key before touching the database.
        if (master_key_hex.has_value()) {
            validate_key_hex(*master_key_hex);
        }

        int rc = sqlite3_open(std::string(path).c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
            if (db_) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("Database: failed to open '" +
                                     std::string(path) + "': " + msg);
        }

        // Apply SQLCipher key immediately after open.
        // The x'' syntax tells SQLCipher to interpret the argument as raw hex
        // bytes rather than a passphrase.  This bypasses KDF key derivation
        // and uses the 32-byte value directly as the AES-256 key.
        // GUARDRAIL: we do not log the hex string.
        if (master_key_hex.has_value()) {
            std::string pragma = "PRAGMA key = \"x'" + *master_key_hex + "'\";";
            char* errmsg = nullptr;
            rc = sqlite3_exec(db_, pragma.c_str(),
                              /*callback=*/nullptr,
                              /*callback_arg=*/nullptr,
                              &errmsg);
            if (rc != SQLITE_OK) {
                std::string msg = errmsg ? errmsg : "(no error message)";
                sqlite3_free(errmsg);
                sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error(
                    "Database: PRAGMA key failed for '" +
                    std::string(path) + "': " + msg);
            }

            // Sanity query — F-2 guardrail: verify the key worked.
            // If the database was encrypted with a different key, SQLCipher
            // returns SQLITE_NOTADB on the first query attempt.
            sqlite3_stmt* sanity_stmt = nullptr;
            rc = sqlite3_prepare_v2(db_,
                "SELECT count(*) FROM sqlite_master;",
                -1, &sanity_stmt, nullptr);
            if (rc == SQLITE_OK) {
                rc = sqlite3_step(sanity_stmt);
                sqlite3_finalize(sanity_stmt);
            } else {
                sqlite3_finalize(sanity_stmt);
            }
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
                sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error(
                    "Database: master key mismatch for '" +
                    std::string(path) + "' (key is wrong or database is corrupt)");
            }
        }

        // Enable foreign key enforcement.
        exec("PRAGMA foreign_keys = ON;");
    }

    ~Database() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    // Move-only.
    Database(Database&& other) noexcept : db_(other.db_) {
        other.db_ = nullptr;
    }
    Database& operator=(Database&& other) noexcept {
        if (this != &other) {
            if (db_) sqlite3_close(db_);
            db_ = other.db_;
            other.db_ = nullptr;
        }
        return *this;
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Execute one or more SQL statements separated by semicolons.
    // Throws std::runtime_error on failure.
    void exec(std::string_view sql) {
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, std::string(sql).c_str(),
                              /*callback=*/nullptr,
                              /*callback_arg=*/nullptr,
                              &errmsg);
        if (rc != SQLITE_OK) {
            std::string msg = errmsg ? errmsg : "(no error message)";
            sqlite3_free(errmsg);
            throw std::runtime_error("Database::exec failed: " + msg +
                                     "\nSQL: " + std::string(sql));
        }
    }

    // Prepare a single SQL statement.  Returns a Statement RAII wrapper.
    // Throws std::runtime_error if the SQL cannot be parsed.
    Statement prepare(std::string_view sql) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.data(),
                                    static_cast<int>(sql.size()),
                                    &stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(
                "Database::prepare failed: " + std::string(sqlite3_errmsg(db_)) +
                "\nSQL: " + std::string(sql));
        }
        return Statement(stmt);
    }

    // Returns the raw sqlite3* handle.  Use for sqlite3_* calls not covered
    // by the public API (e.g. sqlite3_last_insert_rowid).
    sqlite3* raw() noexcept { return db_; }

private:
    sqlite3* db_{nullptr};

    // Validate that hex is exactly 64 lowercase hex characters (32 bytes).
    // Throws std::runtime_error if not.
    // GUARDRAIL: does NOT include the hex value in any error message.
    static void validate_key_hex(const std::string& hex) {
        if (hex.size() != 64) {
            throw std::runtime_error(
                "Database: master_key_hex must be exactly 64 hex characters "
                "(32 bytes); got " + std::to_string(hex.size()) + " characters");
        }
        for (char c : hex) {
            bool valid = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
            if (!valid) {
                throw std::runtime_error(
                    "Database: master_key_hex contains non-hex character");
            }
        }
    }
};
