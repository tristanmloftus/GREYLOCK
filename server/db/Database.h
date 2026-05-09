#pragma once

#include <sqlite3.h>

#include <functional>
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
// Database — RAII wrapper around sqlite3*.
//
// Constructor opens (or creates) the database file at `path`.  Use ":memory:"
// for an in-memory database (useful for tests and schema bootstrapping).
// Destructor closes the database.  Move-only.
//
// Exceptions: constructor throws std::runtime_error on open failure.
//             exec() throws std::runtime_error if the SQL fails.
//             prepare() throws std::runtime_error if the SQL cannot be parsed.
//
// Thread safety: same as SQLite — the Database object is NOT thread-safe.
// Each thread should hold its own Database instance (or serialize access).
// ---------------------------------------------------------------------------
class Database {
public:
    explicit Database(std::string_view path) {
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
};
