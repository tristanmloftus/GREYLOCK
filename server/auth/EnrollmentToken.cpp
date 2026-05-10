#include "EnrollmentToken.h"

#include <sodium.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::auth {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Encode raw bytes as lowercase hex string.
static std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return ss.str();
}

// Decode a lowercase/uppercase hex string to raw bytes.
// Returns empty vector if input is not valid hex or has odd length.
static std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char buf[3] = {hex[i], hex[i + 1], '\0'};
        char* endp = nullptr;
        unsigned long val = std::strtoul(buf, &endp, 16);
        if (endp != buf + 2) return {}; // invalid hex char
        out.push_back(static_cast<uint8_t>(val));
    }
    return out;
}

// BLAKE2b-256 of raw_bytes → 32-byte hash.
static std::vector<std::byte> blake2b256(const uint8_t* data, size_t len) {
    std::vector<std::byte> out(crypto_generichash_BYTES); // 32 bytes
    int rc = crypto_generichash(
        reinterpret_cast<unsigned char*>(out.data()), out.size(),
        data, len,
        nullptr, 0  // no key
    );
    if (rc != 0) {
        throw std::runtime_error("EnrollmentToken: crypto_generichash failed");
    }
    return out;
}

// ---------------------------------------------------------------------------
// mint_enrollment_token
// ---------------------------------------------------------------------------
MintedEnrollmentToken mint_enrollment_token(std::string_view /*email*/,
                                             std::chrono::seconds ttl) {
    // 32 random bytes.
    uint8_t raw_bytes[32];
    randombytes_buf(raw_bytes, sizeof(raw_bytes));

    MintedEnrollmentToken tok;
    tok.raw_token    = bytes_to_hex(raw_bytes, sizeof(raw_bytes));
    tok.token_hash   = blake2b256(raw_bytes, sizeof(raw_bytes));

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    tok.expires_at_unix = now + static_cast<int64_t>(ttl.count());

    return tok;
}

// ---------------------------------------------------------------------------
// persist_enrollment_token
// ---------------------------------------------------------------------------
void persist_enrollment_token(Database& db,
                               const MintedEnrollmentToken& token,
                               std::string_view email) {
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto stmt = db.prepare(
        "INSERT INTO enrollment_tokens "
        "(token_hash, email, created_at_unix, expires_at_unix, consumed_at_unix) "
        "VALUES (?, ?, ?, ?, NULL);"
    );

    sqlite3_bind_blob(stmt.get(), 1,
        token.token_hash.data(),
        static_cast<int>(token.token_hash.size()),
        SQLITE_STATIC);

    sqlite3_bind_text(stmt.get(), 2,
        email.data(), static_cast<int>(email.size()), SQLITE_STATIC);

    sqlite3_bind_int64(stmt.get(), 3, now);
    sqlite3_bind_int64(stmt.get(), 4, token.expires_at_unix);

    int rc = stmt.step();
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            "persist_enrollment_token: INSERT failed");
    }
}

// ---------------------------------------------------------------------------
// consume_enrollment_token
// ---------------------------------------------------------------------------
std::optional<EnrollmentTokenRecord> consume_enrollment_token(
    Database& db,
    std::string_view raw_token,
    int64_t now_unix)
{
    // Decode hex → raw bytes → hash.
    auto raw_bytes = hex_to_bytes(raw_token);
    if (raw_bytes.size() != 32) {
        return std::nullopt; // invalid token format
    }

    auto hash = blake2b256(raw_bytes.data(), raw_bytes.size());

    db.exec("BEGIN IMMEDIATE;");
    try {
        // SELECT the row.
        auto sel = db.prepare(
            "SELECT email, created_at_unix, expires_at_unix, consumed_at_unix "
            "FROM enrollment_tokens WHERE token_hash = ?;"
        );
        sqlite3_bind_blob(sel.get(), 1,
            hash.data(), static_cast<int>(hash.size()), SQLITE_STATIC);

        int rc = sel.step();
        if (rc != SQLITE_ROW) {
            db.exec("ROLLBACK;");
            return std::nullopt; // not found
        }

        std::string email_str;
        const char* email_text = reinterpret_cast<const char*>(
            sqlite3_column_text(sel.get(), 0));
        if (email_text) email_str = email_text;

        int64_t created_at = sqlite3_column_int64(sel.get(), 1);
        int64_t expires_at = sqlite3_column_int64(sel.get(), 2);
        int     consumed_type = sqlite3_column_type(sel.get(), 3);

        // Check expiry.
        if (expires_at <= now_unix) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }

        // Check not yet consumed.
        if (consumed_type != SQLITE_NULL) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }

        // Mark consumed.
        auto upd = db.prepare(
            "UPDATE enrollment_tokens SET consumed_at_unix = ? "
            "WHERE token_hash = ?;"
        );
        sqlite3_bind_int64(upd.get(), 1, now_unix);
        sqlite3_bind_blob(upd.get(), 2,
            hash.data(), static_cast<int>(hash.size()), SQLITE_STATIC);
        int urc = upd.step();
        if (urc != SQLITE_DONE) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }

        db.exec("COMMIT;");

        EnrollmentTokenRecord rec;
        rec.token_hash       = hash;
        rec.email            = email_str;
        rec.created_at_unix  = created_at;
        rec.expires_at_unix  = expires_at;
        rec.consumed_at_unix = now_unix;
        return rec;

    } catch (...) {
        char* errmsg = nullptr;
        sqlite3_exec(db.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
        sqlite3_free(errmsg);
        throw;
    }
}

// ---------------------------------------------------------------------------
// peek_enrollment_token
// ---------------------------------------------------------------------------
std::optional<EnrollmentTokenRecord> peek_enrollment_token(
    Database& db,
    std::string_view raw_token,
    int64_t now_unix)
{
    // Decode hex → raw bytes → hash.
    auto raw_bytes = hex_to_bytes(raw_token);
    if (raw_bytes.size() != 32) {
        return std::nullopt; // invalid token format
    }

    auto hash = blake2b256(raw_bytes.data(), raw_bytes.size());

    // SELECT the row — caller holds the BEGIN IMMEDIATE transaction.
    auto sel = db.prepare(
        "SELECT email, created_at_unix, expires_at_unix, consumed_at_unix "
        "FROM enrollment_tokens WHERE token_hash = ?;"
    );
    sqlite3_bind_blob(sel.get(), 1,
        hash.data(), static_cast<int>(hash.size()), SQLITE_STATIC);

    int rc = sel.step();
    if (rc != SQLITE_ROW) {
        return std::nullopt; // not found
    }

    std::string email_str;
    const char* email_text = reinterpret_cast<const char*>(
        sqlite3_column_text(sel.get(), 0));
    if (email_text) email_str = email_text;

    int64_t created_at   = sqlite3_column_int64(sel.get(), 1);
    int64_t expires_at   = sqlite3_column_int64(sel.get(), 2);
    int     consumed_type = sqlite3_column_type(sel.get(), 3);

    // Check expiry.
    if (expires_at <= now_unix) {
        return std::nullopt;
    }

    // Check not yet consumed.
    if (consumed_type != SQLITE_NULL) {
        return std::nullopt;
    }

    EnrollmentTokenRecord rec;
    rec.token_hash      = hash;
    rec.email           = email_str;
    rec.created_at_unix = created_at;
    rec.expires_at_unix = expires_at;
    rec.consumed_at_unix = 0; // not yet consumed
    return rec;
}

// ---------------------------------------------------------------------------
// mark_enrollment_token_consumed
// ---------------------------------------------------------------------------
bool mark_enrollment_token_consumed(
    Database& db,
    const std::vector<std::byte>& token_hash,
    int64_t now_unix)
{
    auto upd = db.prepare(
        "UPDATE enrollment_tokens SET consumed_at_unix = ? "
        "WHERE token_hash = ?;"
    );
    sqlite3_bind_int64(upd.get(), 1, now_unix);
    sqlite3_bind_blob(upd.get(), 2,
        token_hash.data(), static_cast<int>(token_hash.size()), SQLITE_STATIC);
    int rc = upd.step();
    if (rc != SQLITE_DONE) {
        return false;
    }
    return sqlite3_changes(db.raw()) > 0;
}

} // namespace tf::auth
