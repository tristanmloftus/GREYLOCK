// tests/test_audit_writer.cpp — SqlAuditLog writer integration tests.
//
// Test cases (3):
//   1. Writer_AllEventsAppear — records 5 events; all appear in audit_log.
//   2. Writer_ChainIsVerifiable — after recording, verify_chain returns nullopt.
//   3. Writer_SanitizerRejection_ChainsRejectionEntry — payload with "secret"
//      key is rejected; a sanitizer_rejected_payload entry is chained instead.

#include "server/audit/AuditEvent.h"
#include "server/audit/CanonicalBytes.h"
#include "server/audit/SqlAuditLog.h"
#include "server/db/Database.h"
#include "server/db/Migrations.h"

#include <sqlite3.h>
#include <sodium.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// verify_chain (same implementation as in test_audit_replay.cpp, duplicated
// here to avoid cross-TU linking complexity in unit tests).
// ---------------------------------------------------------------------------

struct VerifyError {
    int64_t seq;
    std::string kind;
    std::string detail;
};

static std::optional<VerifyError> verify_chain(Database& db) {
    auto stmt = db.prepare(
        "SELECT seq, ts_unix_nanos, actor_user_id, actor_kind, domain, "
        "       subject_id, subject_kind, action, outcome, details_json, "
        "       prev_hash, entry_hash "
        "FROM audit_log ORDER BY seq ASC;");

    int64_t expected_seq = 1;
    std::vector<std::byte> expected_prev_hash(32, std::byte{0});

    while (true) {
        int rc = stmt.step();
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return VerifyError{-1, "sqlite_error",
                "step returned " + std::to_string(rc)};
        }

        int64_t seq = sqlite3_column_int64(stmt.get(), 0);
        if (seq != expected_seq) {
            if (seq > expected_seq)
                return VerifyError{expected_seq, "gap",
                    "expected " + std::to_string(expected_seq) +
                    " got " + std::to_string(seq)};
            else
                return VerifyError{seq, "duplicate",
                    "dup seq " + std::to_string(seq)};
        }

        int64_t ts_unix_nanos = sqlite3_column_int64(stmt.get(), 1);

        auto read_text = [&](int col) -> std::string {
            const unsigned char* txt = sqlite3_column_text(stmt.get(), col);
            if (!txt) return {};
            return reinterpret_cast<const char*>(txt);
        };
        auto read_blob = [&](int col) -> std::vector<std::byte> {
            const void* data = sqlite3_column_blob(stmt.get(), col);
            int len = sqlite3_column_bytes(stmt.get(), col);
            if (!data || len == 0) return {};
            const auto* b = static_cast<const std::byte*>(data);
            return {b, b + len};
        };

        std::string actor_user_id = read_text(2);
        std::string actor_kind    = read_text(3);
        std::string domain        = read_text(4);
        std::string subject_id    = read_text(5);
        std::string subject_kind  = read_text(6);
        std::string action        = read_text(7);
        std::string outcome       = read_text(8);

        std::string details_json;
        {
            const void* blob = sqlite3_column_blob(stmt.get(), 9);
            int blen = sqlite3_column_bytes(stmt.get(), 9);
            if (blob && blen > 0)
                details_json.assign(static_cast<const char*>(blob),
                                    static_cast<size_t>(blen));
        }

        std::vector<std::byte> stored_prev_hash  = read_blob(10);
        std::vector<std::byte> stored_entry_hash = read_blob(11);

        int64_t ts_ms = ts_unix_nanos / INT64_C(1'000'000);

        tf::audit::PreCommitEntry pce;
        pce.seq           = seq;
        pce.ts_ms         = ts_ms;
        pce.actor_user_id = actor_user_id;
        pce.actor_kind    = actor_kind;
        pce.domain        = domain;
        pce.subject_id    = subject_id;
        pce.subject_kind  = subject_kind;
        pce.action        = action;
        pce.outcome       = outcome;
        pce.details_json  = details_json;
        pce.prev_hash     = stored_prev_hash.size() == 32
                              ? stored_prev_hash
                              : std::vector<std::byte>(32, std::byte{0});

        auto canonical  = tf::audit::compute_canonical_bytes(pce);
        auto recomputed = tf::audit::compute_entry_hash(canonical);

        if (recomputed != stored_entry_hash)
            return VerifyError{seq, "hash_mismatch",
                "at seq " + std::to_string(seq)};

        if (stored_prev_hash != expected_prev_hash)
            return VerifyError{seq, "prev_hash_mismatch",
                "at seq " + std::to_string(seq)};

        expected_prev_hash = stored_entry_hash;
        ++expected_seq;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Database make_test_db() {
    Database db(":memory:");
    Migrations migrations;
    migrations.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
    migrations.apply_pending(db);
    return db;
}

static tf::audit::AuditEvent make_event(int n) {
    tf::audit::AuditEvent evt;
    evt.ts_ms        = 1000000LL + n * 1000;
    evt.actor_user_id = "user-test";
    evt.actor_kind   = "user";
    evt.domain       = "";
    evt.subject_id   = "subj-" + std::to_string(n);
    evt.subject_kind = "session";
    evt.action       = "test_action_" + std::to_string(n);
    evt.outcome      = "success";
    evt.details      = {{"seq", n}};
    return evt;
}

// ---------------------------------------------------------------------------
// Sodium environment
// ---------------------------------------------------------------------------

class SodiumEnv2 : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gSodiumEnv =
    ::testing::AddGlobalTestEnvironment(new SodiumEnv2());

// ---------------------------------------------------------------------------
// Test 1: All 5 events appear in the table.
// ---------------------------------------------------------------------------
TEST(AuditWriter, Writer_AllEventsAppear) {
    auto db = make_test_db();
    tf::audit::SqlAuditLog log(db);

    for (int i = 1; i <= 5; ++i) {
        ASSERT_NO_THROW(log.record(make_event(i)));
    }

    auto stmt = db.prepare("SELECT COUNT(*) FROM audit_log;");
    ASSERT_EQ(stmt.step(), SQLITE_ROW);
    int count = sqlite3_column_int(stmt.get(), 0);
    EXPECT_EQ(count, 5);
}

// ---------------------------------------------------------------------------
// Test 2: After recording, verify_chain returns nullopt.
// ---------------------------------------------------------------------------
TEST(AuditWriter, Writer_ChainIsVerifiable) {
    auto db = make_test_db();
    tf::audit::SqlAuditLog log(db);

    for (int i = 1; i <= 5; ++i) {
        ASSERT_NO_THROW(log.record(make_event(i)));
    }

    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value())
        << "Chain verification failed: kind=" << (result ? result->kind : "?")
        << " detail=" << (result ? result->detail : "?");
}

// ---------------------------------------------------------------------------
// Test 3: Sanitizer rejection chains a sanitizer_rejected_payload entry.
// ---------------------------------------------------------------------------
TEST(AuditWriter, Writer_SanitizerRejection_ChainsRejectionEntry) {
    auto db = make_test_db();
    tf::audit::SqlAuditLog log(db);

    // Record one good event first.
    ASSERT_NO_THROW(log.record(make_event(1)));

    // Record an event with a "secret" key — sanitizer must reject.
    tf::audit::AuditEvent bad_evt;
    bad_evt.ts_ms        = 2000000LL;
    bad_evt.actor_user_id = "user-test";
    bad_evt.actor_kind   = "user";
    bad_evt.action       = "do_something";
    bad_evt.outcome      = "success";
    bad_evt.details      = {{"secret", "oops-leaked"}};
    ASSERT_NO_THROW(log.record(bad_evt));

    // Should have 2 rows: the good event + the rejection entry.
    auto stmt = db.prepare("SELECT COUNT(*) FROM audit_log;");
    ASSERT_EQ(stmt.step(), SQLITE_ROW);
    int count = sqlite3_column_int(stmt.get(), 0);
    EXPECT_EQ(count, 2) << "Expected 2 rows (1 good + 1 rejection)";

    // The second row should be the rejection entry.
    auto stmt2 = db.prepare("SELECT action FROM audit_log WHERE seq=2;");
    ASSERT_EQ(stmt2.step(), SQLITE_ROW);
    const char* action = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt2.get(), 0));
    EXPECT_STREQ(action, "audit_record_rejected");

    // Chain must still be verifiable.
    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value())
        << "Chain should be verifiable after rejection: kind="
        << (result ? result->kind : "?");
}

// ---------------------------------------------------------------------------
// Test 4: Rows are in seq order with contiguous seq.
// ---------------------------------------------------------------------------
TEST(AuditWriter, Writer_RowsAreContiguousSeq) {
    auto db = make_test_db();
    tf::audit::SqlAuditLog log(db);

    for (int i = 1; i <= 4; ++i) {
        ASSERT_NO_THROW(log.record(make_event(i)));
    }

    auto stmt = db.prepare("SELECT seq FROM audit_log ORDER BY seq ASC;");
    int64_t expected = 1;
    while (stmt.step() == SQLITE_ROW) {
        int64_t seq = sqlite3_column_int64(stmt.get(), 0);
        EXPECT_EQ(seq, expected) << "seq gap at " << expected;
        ++expected;
    }
    EXPECT_EQ(expected, 5) << "Expected 4 rows";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
