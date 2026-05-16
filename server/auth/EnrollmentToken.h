#pragma once

// EnrollmentToken.h — one-shot enrollment token (Phase 3).
//
// Mirrors lib/auth/enrollment-token.ts from the GREYLOCK TS prototype.
//
// Token lifecycle:
//   1. Admin runs: greylock-server --mint-enrollment-token <email>
//   2. Server calls mint() + persist() → prints raw_token to stdout.
//   3. Admin emails raw_token to the new user.
//   4. User POSTs /auth/enroll with {token=raw_token, email, passphrase}.
//   5. Server calls consume() which verifies hash + expiry + not-yet-used.
//
// Storage: only token_hash (BLAKE2b-256 of the raw bytes) is persisted.
// The raw_token is single-use and never stored server-side after issuance.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../server/db/Database.h"

namespace tf::auth {

// ---------------------------------------------------------------------------
// MintedEnrollmentToken
// Returned from mint().  raw_token is given to the new user (never stored).
// token_hash is what goes into the DB.
// ---------------------------------------------------------------------------
struct MintedEnrollmentToken {
    // 64-character lowercase hex string (32 raw bytes → hex-encoded).
    // Hand this to the user exactly as-is; never store it.
    std::string raw_token;

    // BLAKE2b-256 of the 32 raw token bytes.  Stored in enrollment_tokens.
    std::vector<std::byte> token_hash;

    // Unix timestamp when this token expires.
    int64_t expires_at_unix{0};
};

// ---------------------------------------------------------------------------
// EnrollmentTokenRecord
// Returned from consume() on success.
// ---------------------------------------------------------------------------
struct EnrollmentTokenRecord {
    std::vector<std::byte> token_hash;
    std::string email;
    int64_t created_at_unix{0};
    int64_t expires_at_unix{0};
    // consumed_at is set by consume() inside the transaction.
    int64_t consumed_at_unix{0};
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Generate a fresh enrollment token for the given email with the specified TTL.
// Does NOT touch the database — call persist() to store it.
MintedEnrollmentToken mint_enrollment_token(std::string_view email,
                                            std::chrono::seconds ttl);

// Persist the minted token into the enrollment_tokens table.
// Throws std::runtime_error on DB error.
void persist_enrollment_token(Database& db,
                               const MintedEnrollmentToken& token,
                               std::string_view email);

// Consume a raw_token:
//   1. BLAKE2b-256 of the hex-decoded raw_token bytes.
//   2. SELECT the row by token_hash.
//   3. Check expires_at_unix > now_unix AND consumed_at_unix IS NULL.
//   4. UPDATE consumed_at_unix = now_unix.
//   5. Return the record.
// All steps are inside a SQLITE TRANSACTION (BEGIN IMMEDIATE / COMMIT).
// Returns nullopt if: token not found, expired, or already consumed.
std::optional<EnrollmentTokenRecord> consume_enrollment_token(
    Database& db,
    std::string_view raw_token,
    int64_t now_unix);

// Peek at a token record WITHOUT marking it consumed.
// Hashes raw_token, SELECTs the row, checks expiry and not-yet-consumed.
// Returns the record (with email) on success, or nullopt on invalid/expired/consumed.
// Does NOT open or close any transaction — caller must hold one.
std::optional<EnrollmentTokenRecord> peek_enrollment_token(
    Database& db,
    std::string_view raw_token,
    int64_t now_unix);

// Mark a token consumed by its hex-encoded token_hash.
// Runs a single UPDATE; does NOT wrap its own transaction.
// Caller must hold a BEGIN IMMEDIATE transaction.
// Returns false if the UPDATE affected zero rows (token not found).
bool mark_enrollment_token_consumed(
    Database& db,
    const std::vector<std::byte>& token_hash,
    int64_t now_unix);

} // namespace tf::auth
