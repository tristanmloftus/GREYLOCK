// tests/test_audit_chain_canonical_bytes_no_double_count.cpp
//
// AUDIT.md F-4 regression gate: verifies that canonical bytes encode
// tsUnixNanos = ts_ms * 1_000_000 EXACTLY ONCE with no double-counting.
//
// Fixture: ts_ms = 1234567890123
//   Expected tsUnixNanos = 1234567890123 * 1_000_000 = 1234567890123000000
//   Forbidden value:       1234567890246000000  (the F-4 double-count bug)
//
// The test decodes the BE int64 at bytes [8..15] of the canonical output
// and asserts it equals ts_ms * 1_000_000.
//
// Test cases (3):
//   1. TsUnixNanos_No_DoubleCounting — primary F-4 gate.
//   2. SeqField_CorrectBE — seq field is at bytes [0..7], decoded correctly.
//   3. ByteLength_Matches_Expectation — total canonical-bytes length is as expected.

#include "server/audit/CanonicalBytes.h"

#include <sodium.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class SodiumEnv : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gSodiumEnv =
    ::testing::AddGlobalTestEnvironment(new SodiumEnv());

// Decode a big-endian int64 from 8 bytes starting at `data[offset]`.
static int64_t decode_be_int64(const std::vector<std::byte>& data, size_t offset) {
    EXPECT_GE(data.size(), offset + 8) << "buffer too short";
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(data[offset + i]);
    }
    return static_cast<int64_t>(v);
}

// Decode a big-endian uint64 from 8 bytes starting at `data[offset]`.
static uint64_t decode_be_uint64(const std::vector<std::byte>& data, size_t offset) {
    EXPECT_GE(data.size(), offset + 8) << "buffer too short";
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(data[offset + i]);
    }
    return v;
}

// Build a minimal PreCommitEntry with just ts_ms set and the rest empty/zero.
static tf::audit::PreCommitEntry make_entry(int64_t seq, int64_t ts_ms) {
    tf::audit::PreCommitEntry e;
    e.seq           = seq;
    e.ts_ms         = ts_ms;
    e.actor_user_id = "";
    e.actor_kind    = "system";
    e.domain        = "";
    e.subject_id    = "";
    e.subject_kind  = "";
    e.action        = "test_action";
    e.outcome       = "success";
    e.details_json  = "{}";
    e.prev_hash     = std::vector<std::byte>(32, std::byte{0});
    return e;
}

// ---------------------------------------------------------------------------
// Test 1: tsUnixNanos = ts_ms * 1_000_000 — F-4 primary gate.
// ---------------------------------------------------------------------------
TEST(AuditCanonicalBytes, TsUnixNanos_No_DoubleCounting) {
    const int64_t ts_ms = 1234567890123LL;
    const int64_t expected_ts_nanos = ts_ms * INT64_C(1'000'000);
    // The F-4 double-count bug would produce ts_ms * 2 * 1_000_000.
    const int64_t forbidden_ts_nanos = ts_ms * INT64_C(2'000'000);

    auto entry = make_entry(1, ts_ms);
    auto canonical = tf::audit::compute_canonical_bytes(entry);

    ASSERT_GE(canonical.size(), 16u) << "canonical bytes too short";

    // tsUnixNanos is at bytes [8..15] (after the 8-byte seq field).
    int64_t decoded_nanos = decode_be_int64(canonical, 8);

    EXPECT_EQ(decoded_nanos, expected_ts_nanos)
        << "tsUnixNanos mismatch: got " << decoded_nanos
        << ", expected " << expected_ts_nanos
        << " (F-4: ts_ms * 1_000_000 exactly once)";

    EXPECT_NE(decoded_nanos, forbidden_ts_nanos)
        << "F-4 double-count bug detected: got " << decoded_nanos
        << " which equals ts_ms * 2_000_000";

    // Additional explicit check: 1234567890123 * 1_000_000.
    EXPECT_EQ(decoded_nanos, 1234567890123000000LL);
}

// ---------------------------------------------------------------------------
// Test 2: seq field at bytes [0..7], decoded correctly as big-endian uint64.
// ---------------------------------------------------------------------------
TEST(AuditCanonicalBytes, SeqField_CorrectBE) {
    const int64_t seq = 42;
    auto entry = make_entry(seq, 1000LL);
    auto canonical = tf::audit::compute_canonical_bytes(entry);

    ASSERT_GE(canonical.size(), 8u);
    uint64_t decoded_seq = decode_be_uint64(canonical, 0);
    EXPECT_EQ(decoded_seq, static_cast<uint64_t>(seq));
}

// ---------------------------------------------------------------------------
// Test 3: byte length matches expectation for a known fixture.
// ---------------------------------------------------------------------------
TEST(AuditCanonicalBytes, ByteLength_Matches_Expectation) {
    // With these fields:
    //   seq (8) + tsUnixNanos (8) + actorUserId ("" + NUL = 1) +
    //   actorKind ("system" + NUL = 7) + domain ("" + NUL = 1) +
    //   subjectId ("" + NUL = 1) + subjectKind ("" + NUL = 1) +
    //   action ("test_action" + NUL = 12) + outcome ("success" + NUL = 8) +
    //   detailsJson (len32be=4 + "{}" bytes=2 = 6) +
    //   prevHash (32 bytes)
    // = 8 + 8 + 1 + 7 + 1 + 1 + 1 + 12 + 8 + 6 + 32 = 85

    auto entry = make_entry(1, 1000LL);
    auto canonical = tf::audit::compute_canonical_bytes(entry);

    size_t expected_len =
        8  +   // seq
        8  +   // tsUnixNanos
        1  +   // actorUserId "" + NUL
        7  +   // actorKind "system" (6) + NUL
        1  +   // domain "" + NUL
        1  +   // subjectId "" + NUL
        1  +   // subjectKind "" + NUL
        12 +   // action "test_action" (11) + NUL
        8  +   // outcome "success" (7) + NUL
        4  +   // len32be of details_json
        2  +   // "{}" bytes
        32;    // prevHash

    EXPECT_EQ(canonical.size(), expected_len)
        << "Canonical byte length mismatch: got " << canonical.size()
        << ", expected " << expected_len;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
