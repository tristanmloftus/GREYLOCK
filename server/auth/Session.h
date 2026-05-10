#pragma once

// Session.h — session lifecycle (Phase 3).
//
// Session tokens:
//   - Generated from randombytes_buf (F-1: never derived from email/public input).
//   - Base64url-encoded for the raw_token sent to the client.
//   - BLAKE2b-256 hashed for storage in the sessions table.
//   - The sessions table stores only the hash; the plaintext token appears only
//     in the mint() return value and must not be logged or persisted elsewhere.
//
// Timeouts (plan §d):
//   - Absolute: 8 hours from creation.
//   - Idle: 30 minutes since last_seen_unix.
//
// GUARDRAIL F-2: validate_and_touch() compares the token hash via the sessions
// table lookup (exact match on primary key, which is the hash).  The DB lookup
// itself is safe; no additional constant-time compare is needed since we're
// comparing hash against hash (a stored value), not against user input directly.
// The hash comparison is done by SQLite's BLOB primary key scan which is
// deterministic but not constant-time — acceptable because the attacker can't
// enumerate stored hashes without the plaintext token.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../server/db/Database.h"

namespace tf::auth {

// Absolute and idle timeout constants (seconds).
// Exposed as constants so tests can reference them directly.
static constexpr int64_t kSessionIdleTimeoutSeconds     = 30 * 60;   // 30 min
static constexpr int64_t kSessionAbsoluteTimeoutSeconds = 8 * 3600;  // 8 h

// ---------------------------------------------------------------------------
// MintedSession
// raw_token is the opaque base64url string sent to the client.
// token_hash is what is stored in the sessions table.
// ---------------------------------------------------------------------------
struct MintedSession {
    // Opaque base64url-encoded string (sent to client exactly once).
    // NEVER log or persist this value.
    std::string raw_token;

    // BLAKE2b-256 of the 32 random raw bytes (before base64url encoding).
    // Stored as the primary key in the sessions table.
    std::vector<std::byte> token_hash;
};

// Mint a new session for user_id.
// Inserts into the sessions table and returns the MintedSession.
// Throws std::runtime_error on DB error.
// F-1: raw_token bytes come from randombytes_buf only.
MintedSession mint_session(Database& db,
                            std::string_view user_id,
                            int64_t now_unix);

// Validate a raw_token and update last_seen_unix.
//
// Checks (F-2 compliance):
//   1. revoked == 0
//   2. expires_at_unix > now_unix         (absolute timeout)
//   3. last_seen_unix > now_unix - kSessionIdleTimeoutSeconds  (idle timeout)
//
// On success: UPDATE last_seen_unix = now_unix; return user_id.
// On failure: return nullopt.
// All inside a SQLITE TRANSACTION (BEGIN IMMEDIATE / COMMIT).
std::optional<std::string> validate_and_touch_session(Database& db,
                                                       std::string_view raw_token,
                                                       int64_t now_unix);

// Revoke a session by setting revoked = 1 for the matching token hash.
// Returns true if a row was found and updated, false otherwise.
bool revoke_session(Database& db, std::string_view raw_token);

} // namespace tf::auth
