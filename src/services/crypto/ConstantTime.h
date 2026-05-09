#pragma once

// ConstantTime.h — Constant-time comparison helpers.
//
// GUARDRAIL F-2: ALL byte comparisons for security-sensitive data MUST use
// sodium_memcmp, never std::memcmp, std::equal, or hand-written loops.
// sodium_memcmp is the single authoritative constant-time comparator here.
//
// Semantics:
//   - If the two spans/strings differ in SIZE, we return false immediately.
//     This length check is NOT constant-time across different sizes, which is
//     CORRECT: constant-time byte comparison is only meaningful (and possible)
//     when the lengths are equal. Leaking "the lengths differ" is acceptable
//     for the use cases in this project (AEAD tag comparison is handled by
//     libsodium internally; these helpers are for higher-level logic).
//   - If lengths are equal, sodium_memcmp compares every byte in constant time.

#include <cstddef>
#include <span>
#include <string_view>

#include <sodium.h>

namespace tf::crypto::constant_time {

// Compare two byte spans in constant time.
// Returns true iff a.size() == b.size() AND all bytes are equal.
inline bool equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) {
        return false; // length leak is acceptable; see header comment
    }
    if (a.empty()) {
        return true; // sodium_memcmp(ptr, ptr, 0) is defined but skip the call
    }
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

// Convenience overload for string_view (e.g. comparing hex digests).
inline bool equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace tf::crypto::constant_time
