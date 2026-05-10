#pragma once

// PassphraseHash.h — Argon2id passphrase hashing (Phase 3).
//
// Uses libsodium's crypto_pwhash_str / crypto_pwhash_str_verify which produce
// a self-describing encoded string (includes algorithm, opslimit, memlimit,
// and salt) so future tuning in Phase 6 does not break existing hashes.
//
// GUARDRAIL F-2: verify() delegates to crypto_pwhash_str_verify which is
// constant-time per libsodium's documented contract.
//
// Tuning: OPSLIMIT_MODERATE and MEMLIMIT_MODERATE are the defaults.
// For Phase 6 load testing, raise to SENSITIVE or lower to INTERACTIVE.
// The current defaults target ~0.7 s / 256 MiB RAM on a modern server.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace tf::auth {

// Hash a passphrase using Argon2id.
//
// Returns a self-describing hash string encoded as bytes (the ASCII chars of
// libsodium's crypto_pwhash_str output, including the trailing NUL terminator
// so stored_hash.data() can be passed directly to crypto_pwhash_str_verify).
//
// The returned vector is crypto_pwhash_STRBYTES long.
//
// Throws std::runtime_error if hashing fails (OOM, libsodium not initialized).
std::vector<std::byte> hash_passphrase(std::string_view passphrase);

// Verify a passphrase against a stored hash produced by hash_passphrase().
//
// Returns true iff the passphrase matches.  False on mismatch or if the
// stored_hash blob is malformed.
//
// Constant-time per libsodium's contract on crypto_pwhash_str_verify.
bool verify_passphrase(std::string_view passphrase,
                       std::span<const std::byte> stored_hash);

} // namespace tf::auth
