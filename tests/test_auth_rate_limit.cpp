// test_auth_rate_limit.cpp — unit tests for the in-process rate-limit
// bucket map in AuthHandlers.cpp (Phase 4.D corrective deferral).
//
// Coverage:
//   - Normal behavior: kRateLimitMax requests within the window pass; the
//     (kRateLimitMax+1)th trips.
//   - Window expiration: after kRateLimitWindowSecs, the counter resets.
//   - Bounded growth: the map will not exceed kMaxRateBuckets.  If the cap
//     is reached without expired entries to sweep, new keys fail closed.
//   - Eviction on insert: expired buckets are evicted when at the cap.
//
// We reach the rate_limit_check() and helpers via the test-only namespace
// declared in server/auth/AuthRateLimitInternal.h.  No HTTP plumbing.

#include "../server/auth/AuthRateLimitInternal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using tf::auth::internal::rate_limit_check;
using tf::auth::internal::rate_limit_reset_for_tests;
using tf::auth::internal::rate_limit_bucket_count_for_tests;
using tf::auth::internal::kRateLimitMax;
using tf::auth::internal::kRateLimitWindowSecs;
using tf::auth::internal::kMaxRateBuckets;

class AuthRateLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        rate_limit_reset_for_tests();
    }
};

// ---------------------------------------------------------------------------
// Normal flow: first kRateLimitMax requests pass, the next is tripped.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, FifthAttemptTripsLimit) {
    const std::string key = "auth_login:user@example.com";
    const int64_t t = 1'700'000'000;

    for (int i = 0; i < kRateLimitMax; ++i) {
        EXPECT_FALSE(rate_limit_check(key, t))
            << "attempt " << (i + 1) << " should NOT be limited";
    }
    EXPECT_TRUE(rate_limit_check(key, t))
        << "attempt " << (kRateLimitMax + 1) << " should be limited";
}

// ---------------------------------------------------------------------------
// Window expiry: after kRateLimitWindowSecs the bucket resets.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, WindowExpiryResetsBucket) {
    const std::string key = "auth_login:user@example.com";
    const int64_t t0 = 1'700'000'000;

    for (int i = 0; i < kRateLimitMax; ++i) {
        EXPECT_FALSE(rate_limit_check(key, t0));
    }
    EXPECT_TRUE(rate_limit_check(key, t0));

    // Advance past the window — the bucket should reset on next call.
    const int64_t t1 = t0 + kRateLimitWindowSecs + 1;
    EXPECT_FALSE(rate_limit_check(key, t1))
        << "first call after window expiry should pass";
}

// ---------------------------------------------------------------------------
// Per-key isolation: rate-limit is keyed by bucket_key, not global.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, DifferentKeysIsolated) {
    const int64_t t = 1'700'000'000;
    for (int i = 0; i < kRateLimitMax; ++i) {
        EXPECT_FALSE(rate_limit_check("auth_login:a@example.com", t));
    }
    // The first key is tripped, but a different key should still pass.
    EXPECT_TRUE(rate_limit_check("auth_login:a@example.com", t));
    EXPECT_FALSE(rate_limit_check("auth_login:b@example.com", t));
}

// ---------------------------------------------------------------------------
// Bounded growth: the map will not grow past kMaxRateBuckets when all
// existing buckets are still within their window.  The (kMaxRateBuckets+1)th
// distinct key is denied (fail-closed) and the map size stays at the cap.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, MapSaturationDeniesNewKey) {
    const int64_t t = 1'700'000'000;

    // Fill the map exactly to kMaxRateBuckets with active (non-expired) keys.
    for (std::size_t i = 0; i < kMaxRateBuckets; ++i) {
        std::string key = "auth_login:fill_" + std::to_string(i);
        EXPECT_FALSE(rate_limit_check(key, t));
    }
    EXPECT_EQ(rate_limit_bucket_count_for_tests(), kMaxRateBuckets);

    // A new, never-seen key should fail closed.
    EXPECT_TRUE(rate_limit_check("auth_login:overflow@example.com", t));
    EXPECT_EQ(rate_limit_bucket_count_for_tests(), kMaxRateBuckets)
        << "saturated map must not grow past the cap";
}

// ---------------------------------------------------------------------------
// Existing keys still work even when the map is saturated.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, SaturatedMapStillServicesExistingKeys) {
    const int64_t t = 1'700'000'000;
    for (std::size_t i = 0; i < kMaxRateBuckets; ++i) {
        std::string key = "auth_login:fill_" + std::to_string(i);
        EXPECT_FALSE(rate_limit_check(key, t));
    }
    // Existing key still has (kRateLimitMax - 1) attempts left.
    for (int i = 1; i < kRateLimitMax; ++i) {
        EXPECT_FALSE(rate_limit_check("auth_login:fill_0", t));
    }
    EXPECT_TRUE(rate_limit_check("auth_login:fill_0", t))
        << "existing key should still trip at kRateLimitMax+1";
}

// ---------------------------------------------------------------------------
// Eviction: when the map is at the cap, expired buckets are swept to make
// room for a new key — and the new key is accepted.
// ---------------------------------------------------------------------------
TEST_F(AuthRateLimitTest, ExpiredBucketsEvictedOnInsertAtCap) {
    const int64_t t0 = 1'700'000'000;

    // Fill the map.
    for (std::size_t i = 0; i < kMaxRateBuckets; ++i) {
        std::string key = "auth_login:old_" + std::to_string(i);
        EXPECT_FALSE(rate_limit_check(key, t0));
    }
    EXPECT_EQ(rate_limit_bucket_count_for_tests(), kMaxRateBuckets);

    // Advance time past the window so every existing bucket is expired.
    const int64_t t1 = t0 + kRateLimitWindowSecs + 1;

    // A new key at t1 must succeed (sweep makes room for it).
    EXPECT_FALSE(rate_limit_check("auth_login:fresh@example.com", t1));
    EXPECT_LE(rate_limit_bucket_count_for_tests(), kMaxRateBuckets)
        << "after eviction the cap is still respected";
}

} // anonymous namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
