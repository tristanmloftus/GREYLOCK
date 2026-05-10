#include "Session.h"

#include <sodium.h>
#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::auth {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// BLAKE2b-256 of input bytes → 32-byte hash vector.
static std::vector<std::byte> blake2b256_session(const uint8_t* data, size_t len) {
    std::vector<std::byte> out(crypto_generichash_BYTES); // 32 bytes
    int rc = crypto_generichash(
        reinterpret_cast<unsigned char*>(out.data()), out.size(),
        data, len,
        nullptr, 0
    );
    if (rc != 0) {
        throw std::runtime_error("Session: crypto_generichash failed");
    }
    return out;
}

// Base64url-encode bytes (RFC 4648 §5, no padding).
// Uses libsodium's sodium_bin2base64 with sodium_base64_VARIANT_URLSAFE_NO_PADDING.
static std::string base64url_encode(const uint8_t* data, size_t len) {
    size_t max_len = sodium_base64_encoded_len(len,
        sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string out(max_len, '\0');
    sodium_bin2base64(out.data(), out.size(),
        data, len,
        sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    // sodium_bin2base64 NUL-terminates; trim the trailing NUL.
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

// Decode a base64url string to raw bytes.
// Returns empty vector on failure.
static std::vector<uint8_t> base64url_decode(std::string_view b64) {
    size_t bin_max = b64.size(); // decoded is always <= encoded
    std::vector<uint8_t> out(bin_max);
    size_t bin_len = 0;
    int rc = sodium_base642bin(
        out.data(), out.size(),
        b64.data(), b64.size(),
        nullptr, // ignore whitespace? no — strict
        &bin_len,
        nullptr,
        sodium_base64_VARIANT_URLSAFE_NO_PADDING
    );
    if (rc != 0) {
        return {};
    }
    out.resize(bin_len);
    return out;
}

// ---------------------------------------------------------------------------
// mint_session
// ---------------------------------------------------------------------------
MintedSession mint_session(Database& db,
                            std::string_view user_id,
                            int64_t now_unix)
{
    // F-1: generate 32 random bytes.
    uint8_t raw_bytes[32];
    randombytes_buf(raw_bytes, sizeof(raw_bytes));

    MintedSession s;
    s.raw_token  = base64url_encode(raw_bytes, sizeof(raw_bytes));
    s.token_hash = blake2b256_session(raw_bytes, sizeof(raw_bytes));

    // The sessions table primary key is `id TEXT`, but we store the hash as
    // a hex string to keep TEXT primary key consistent with other tables.
    // Actually the schema says `id TEXT PRIMARY KEY` — we store the hash blob
    // hex-encoded as the id.  This allows the SELECT-by-id path in validate.
    //
    // Alternative: use the BLOB directly.  The schema says TEXT for id, so
    // we hex-encode the hash as the sessions.id.

    // Hex-encode the hash as the session id.
    char id_hex[65];
    sodium_bin2hex(id_hex, sizeof(id_hex),
        reinterpret_cast<const unsigned char*>(s.token_hash.data()),
        s.token_hash.size());
    std::string session_id(id_hex);

    int64_t expires_at = now_unix + kSessionAbsoluteTimeoutSeconds;

    auto stmt = db.prepare(
        "INSERT INTO sessions "
        "(id, user_id, created_at_unix, last_seen_unix, expires_at_unix, revoked) "
        "VALUES (?, ?, ?, ?, ?, 0);"
    );

    sqlite3_bind_text(stmt.get(), 1,
        session_id.data(), static_cast<int>(session_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2,
        user_id.data(), static_cast<int>(user_id.size()), SQLITE_STATIC);
    sqlite3_bind_int64(stmt.get(), 3, now_unix);
    sqlite3_bind_int64(stmt.get(), 4, now_unix);
    sqlite3_bind_int64(stmt.get(), 5, expires_at);

    int rc = stmt.step();
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("mint_session: INSERT into sessions failed");
    }

    return s;
}

// ---------------------------------------------------------------------------
// validate_and_touch_session
// ---------------------------------------------------------------------------
std::optional<std::string> validate_and_touch_session(Database& db,
                                                        std::string_view raw_token,
                                                        int64_t now_unix)
{
    // Decode raw_token → raw bytes → hash → hex id.
    auto raw_bytes = base64url_decode(raw_token);
    if (raw_bytes.size() != 32) {
        return std::nullopt;
    }

    auto hash = blake2b256_session(raw_bytes.data(), raw_bytes.size());

    char id_hex[65];
    sodium_bin2hex(id_hex, sizeof(id_hex),
        reinterpret_cast<const unsigned char*>(hash.data()), hash.size());
    std::string session_id(id_hex);

    db.exec("BEGIN IMMEDIATE;");
    try {
        auto sel = db.prepare(
            "SELECT user_id, expires_at_unix, last_seen_unix, revoked "
            "FROM sessions WHERE id = ?;"
        );
        sqlite3_bind_text(sel.get(), 1,
            session_id.data(), static_cast<int>(session_id.size()), SQLITE_STATIC);

        int rc = sel.step();
        if (rc != SQLITE_ROW) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }

        const char* uid_text = reinterpret_cast<const char*>(
            sqlite3_column_text(sel.get(), 0));
        std::string uid = uid_text ? uid_text : "";

        int64_t expires_at  = sqlite3_column_int64(sel.get(), 1);
        int64_t last_seen   = sqlite3_column_int64(sel.get(), 2);
        int     revoked     = sqlite3_column_int  (sel.get(), 3);

        // F-2: check all conditions against the wall clock.
        if (revoked != 0) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }
        if (expires_at <= now_unix) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }
        if (last_seen <= now_unix - kSessionIdleTimeoutSeconds) {
            db.exec("ROLLBACK;");
            return std::nullopt;
        }

        // Touch last_seen.
        auto upd = db.prepare(
            "UPDATE sessions SET last_seen_unix = ? WHERE id = ?;"
        );
        sqlite3_bind_int64(upd.get(), 1, now_unix);
        sqlite3_bind_text(upd.get(), 2,
            session_id.data(), static_cast<int>(session_id.size()), SQLITE_STATIC);
        upd.step();

        db.exec("COMMIT;");
        return uid;

    } catch (...) {
        char* errmsg = nullptr;
        sqlite3_exec(db.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
        sqlite3_free(errmsg);
        throw;
    }
}

// ---------------------------------------------------------------------------
// revoke_session
// ---------------------------------------------------------------------------
bool revoke_session(Database& db, std::string_view raw_token) {
    auto raw_bytes = base64url_decode(raw_token);
    if (raw_bytes.size() != 32) {
        return false;
    }

    auto hash = blake2b256_session(raw_bytes.data(), raw_bytes.size());

    char id_hex[65];
    sodium_bin2hex(id_hex, sizeof(id_hex),
        reinterpret_cast<const unsigned char*>(hash.data()), hash.size());
    std::string session_id(id_hex);

    auto stmt = db.prepare(
        "UPDATE sessions SET revoked = 1 WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        session_id.data(), static_cast<int>(session_id.size()), SQLITE_STATIC);
    stmt.step();

    return sqlite3_changes(db.raw()) > 0;
}

} // namespace tf::auth
