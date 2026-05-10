#pragma once

// Totp.h — RFC 6238 TOTP implementation (Phase 3).
//
// Algorithm: HMAC-SHA1, 30-second period, 6 digits.
// Secret: 20 random bytes (standard size for TOTP per RFC 4226).
//
// GUARDRAIL F-2: verify_code() compares candidates via constant-time compare
// using tf::crypto::constant_time::equal on the integer representations
// encoded as byte arrays.  No short-circuit on first match.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tf::auth {

// Generate a 20-byte random TOTP secret (randombytes_buf).
std::vector<std::byte> generate_totp_secret();

// Encode a TOTP secret as base32 (RFC 4648, no padding variant used by
// most authenticator apps).  Returns uppercase base32 string.
std::string totp_secret_to_base32(std::span<const std::byte> secret);

// Build an otpauth:// provisioning URI for QR-code import.
//
//   otpauth://totp/<issuer>:<label>?secret=<base32>&issuer=<issuer>
//                                  &algorithm=SHA1&digits=6&period=30
//
// Both label and issuer are URL-encoded.
std::string make_totp_provisioning_uri(std::string_view label,
                                       std::string_view issuer,
                                       std::span<const std::byte> secret);

// Compute the 6-digit TOTP code for the given secret and unix timestamp.
//
// counter = unix_seconds / 30  (RFC 6238 §4.2)
// HMAC-SHA1(secret, counter as big-endian uint64)
// Dynamic truncation → 31-bit integer → modulo 10^6
//
// Returns an integer in [0, 999999].  Zero-pad to 6 digits at display time.
int compute_totp_code(std::span<const std::byte> secret, int64_t unix_seconds);

// Verify a 6-digit TOTP code against the current window ± skew_window steps.
//
// Checks each step in [now - skew_window, ..., now + skew_window].
// All candidates are compared in constant-time (no short-circuit on first hit).
//
// skew_window = 1 means ±30s (one step before and one after current).
bool verify_totp_code(std::span<const std::byte> secret,
                      int64_t unix_seconds,
                      int code,
                      int skew_window = 1);

} // namespace tf::auth
