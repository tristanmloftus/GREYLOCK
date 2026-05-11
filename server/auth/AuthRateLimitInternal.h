#pragma once

// AuthRateLimitInternal.h — INTERNAL test-only surface for the in-process
// rate-limit logic used by /auth/login (Phase 4.D corrective deferral).
//
// This header exists ONLY to make rate_limit_check() reachable from a unit
// test binary without spinning up an HTTP server.  The function and its
// supporting reset helper live in AuthHandlers.cpp's translation unit so
// the production codepath is unchanged.
//
// Production callers should NEVER include this header; they reach the
// rate-limit logic implicitly via register_auth_handlers().

#include <cstdint>
#include <cstddef>
#include <string>

namespace tf::auth::internal {

// Tunables (kept in sync with the constants in AuthHandlers.cpp).
constexpr int     kRateLimitMax        = 5;
constexpr int64_t kRateLimitWindowSecs = 15 * 60;  // 15 minutes
constexpr std::size_t kMaxRateBuckets  = 10000;

// Returns true if the request keyed by bucket_key at unix-time t_unix
// should be rate-limited (i.e. either the per-key counter is tripped or
// the global bucket map is saturated and could not evict).  Otherwise
// increments the per-key counter and returns false.
//
// Guarantees (see AuthHandlers.cpp for implementation):
//   - Per-bucket window of kRateLimitWindowSecs; expired buckets reset.
//   - Global cap of kMaxRateBuckets entries; opportunistic sweep of
//     expired buckets occurs when at the cap.  If the map is still at
//     the cap after the sweep, the call fails closed (returns true) and
//     emits a warning so the saturation itself becomes a signal.
bool rate_limit_check(const std::string& bucket_key, int64_t t_unix);

// Test-only: clear the global rate-limit map so tests start from a known
// state.  Not called from production code.
void rate_limit_reset_for_tests();

// Test-only: returns the current number of buckets in the map.
std::size_t rate_limit_bucket_count_for_tests();

} // namespace tf::auth::internal
