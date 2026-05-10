// tests/test_audit_replay.cpp — replay-verification tests (SHIPS BEFORE writer).
//
// The verify_chain() function walks audit_log rows in seq-ascending order,
// recomputes each entry_hash from canonical bytes, and asserts:
//   1. entry_hash matches stored value (byte-for-byte).
//   2. prev_hash matches the previous row's entry_hash (or 32 zero bytes for seq=1).
//   3. seq is contiguous and starts at 1.
//
// Test fixtures use raw SQL INSERTs (no SqlAuditLog writer) to hand-roll chains.
// This keeps the verifier test independent of the writer implementation.
//
// Test cases (7):
//   1. Verify_CleanChain_Returns_Nullopt — 3 entries, all valid.
//   2. Verify_TamperedDetailsJson_DetectsTamper — flip a byte in details_json.
//   3. Verify_TamperedPrevHash_DetectsTamper — flip a byte in prev_hash.
//   4. Verify_MissingSeq_DetectsGap — delete row with seq=2.
//   5. Verify_DuplicateSeq_DetectsDup — UNIQUE constraint prevents this; verify behavior.
//   6. Verify_EmptyChain_Returns_Nullopt — no rows, valid empty chain.
//   7. Verify_SingleRowChain_Returns_Nullopt — 1 entry with prev_hash = zeros.

#include "server/audit/CanonicalBytes.h"
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
// verify_chain implementation
// ---------------------------------------------------------------------------

struct VerifyError {
    int64_t seq;        // row where the error was detected
    std::string kind;   // "hash_mismatch" | "prev_hash_mismatch" | "gap" | "duplicate"
    std::string detail;
};

// Verify the entire audit_log chain.
// Returns nullopt on success, VerifyError describing the first detected problem.
static std::optional<VerifyError> verify_chain(Database& db) {
    // Walk rows in seq ascending order.
    auto stmt = db.prepare(
        "SELECT seq, ts_unix_nanos, actor_user_id, actor_kind, domain, "
        "       subject_id, subject_kind, action, outcome, details_json, "
        "       prev_hash, entry_hash "
        "FROM audit_log ORDER BY seq ASC;");

    int64_t expected_seq = 1;
    std::vector<std::byte> expected_prev_hash(32, std::byte{0});
    std::vector<std::byte> last_entry_hash;
    bool first = true;
    int64_t last_seq = -1;

    while (true) {
        int rc = stmt.step();
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return VerifyError{-1, "sqlite_error",
                "sqlite3_step returned " + std::to_string(rc)};
        }

        int64_t seq = sqlite3_column_int64(stmt.get(), 0);

        // Check for gap.
        if (seq != expected_seq) {
            if (seq > expected_seq) {
                return VerifyError{expected_seq, "gap",
                    "expected seq " + std::to_string(expected_seq) +
                    " but got " + std::to_string(seq)};
            } else {
                // seq < expected_seq means duplicate (shouldn't happen with UNIQUE PRIMARY KEY).
                return VerifyError{seq, "duplicate",
                    "duplicate seq " + std::to_string(seq)};
            }
        }

        // Read stored fields.
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

        // details_json: may be stored as BLOB or TEXT.
        std::string details_json;
        {
            const void* blob = sqlite3_column_blob(stmt.get(), 9);
            int blen = sqlite3_column_bytes(stmt.get(), 9);
            if (blob && blen > 0) {
                details_json.assign(static_cast<const char*>(blob),
                                    static_cast<size_t>(blen));
            }
        }

        std::vector<std::byte> stored_prev_hash  = read_blob(10);
        std::vector<std::byte> stored_entry_hash = read_blob(11);

        // Reconstruct ts_ms from ts_unix_nanos (reverse F-4 multiply).
        int64_t ts_ms = ts_unix_nanos / INT64_C(1'000'000);

        // Build PreCommitEntry to recompute canonical bytes.
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

        auto canonical   = tf::audit::compute_canonical_bytes(pce);
        auto recomputed  = tf::audit::compute_entry_hash(canonical);

        // Assert entry_hash matches recomputed hash.
        if (recomputed != stored_entry_hash) {
            return VerifyError{seq, "hash_mismatch",
                "entry_hash does not match recomputed hash at seq " +
                std::to_string(seq)};
        }

        // Assert prev_hash matches the previous row's entry_hash.
        if (stored_prev_hash != expected_prev_hash) {
            return VerifyError{seq, "prev_hash_mismatch",
                "prev_hash mismatch at seq " + std::to_string(seq)};
        }

        // Advance state.
        expected_prev_hash = stored_entry_hash;
        last_entry_hash    = stored_entry_hash;
        last_seq           = seq;
        ++expected_seq;
        first = false;
        (void)first;
    }

    return std::nullopt; // chain is valid
}

// ---------------------------------------------------------------------------
// Helpers for hand-rolling chain entries in tests
// ---------------------------------------------------------------------------

// Helper: apply M001 to a fresh in-memory database.
static Database make_test_db() {
    Database db(":memory:");
    Migrations migrations;
    migrations.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
    migrations.apply_pending(db);
    return db;
}

// Helper: compute a valid entry_hash for given fields, for hand-rolling a chain.
// ts_unix_nanos = ts_ms * 1_000_000 (caller passes ts_ms).
static std::vector<std::byte> compute_hash_for_row(
        int64_t seq,
        int64_t ts_ms,
        const std::string& actor_user_id,
        const std::string& actor_kind,
        const std::string& domain,
        const std::string& subject_id,
        const std::string& subject_kind,
        const std::string& action,
        const std::string& outcome,
        const std::string& details_json,
        const std::vector<std::byte>& prev_hash) {
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
    pce.prev_hash     = prev_hash;
    auto canonical = tf::audit::compute_canonical_bytes(pce);
    return tf::audit::compute_entry_hash(canonical);
}

// Helper: insert one hand-rolled row directly via SQL.
// ts_unix_nanos is already computed (ts_ms * 1_000_000).
static void insert_row(Database& db,
                        int64_t seq,
                        int64_t ts_unix_nanos,
                        const std::string& actor_user_id,
                        const std::string& actor_kind,
                        const std::string& action,
                        const std::string& outcome,
                        const std::string& details_json,
                        const std::vector<std::byte>& prev_hash,
                        const std::vector<std::byte>& entry_hash) {
    auto stmt = db.prepare(
        "INSERT INTO audit_log "
        "(seq, ts_unix_nanos, actor_user_id, actor_kind, domain, subject_id, "
        " subject_kind, action, outcome, details_json, prev_hash, entry_hash) "
        "VALUES (?, ?, ?, ?, '', '', '', ?, ?, ?, ?, ?);");

    sqlite3_bind_int64(stmt.get(), 1, seq);
    sqlite3_bind_int64(stmt.get(), 2, ts_unix_nanos);

    auto bind_text_or_null = [&](int idx, const std::string& s) {
        if (s.empty()) sqlite3_bind_null(stmt.get(), idx);
        else sqlite3_bind_text(stmt.get(), idx, s.c_str(),
                               static_cast<int>(s.size()), SQLITE_STATIC);
    };
    bind_text_or_null(3, actor_user_id);
    sqlite3_bind_text(stmt.get(), 4, actor_kind.c_str(),
                      static_cast<int>(actor_kind.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 5, action.c_str(),
                      static_cast<int>(action.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 6, outcome.c_str(),
                      static_cast<int>(outcome.size()), SQLITE_STATIC);

    if (details_json.empty()) {
        sqlite3_bind_null(stmt.get(), 7);
    } else {
        sqlite3_bind_blob(stmt.get(), 7, details_json.data(),
                          static_cast<int>(details_json.size()), SQLITE_STATIC);
    }

    sqlite3_bind_blob(stmt.get(), 8,
                      prev_hash.data(), static_cast<int>(prev_hash.size()),
                      SQLITE_STATIC);
    sqlite3_bind_blob(stmt.get(), 9,
                      entry_hash.data(), static_cast<int>(entry_hash.size()),
                      SQLITE_STATIC);

    int rc = stmt.step();
    ASSERT_EQ(rc, SQLITE_DONE) << "insert_row failed at seq=" << seq;
}

// ---------------------------------------------------------------------------
// Sodium environment
// ---------------------------------------------------------------------------

class SodiumEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new SodiumEnvironment());

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class AuditReplayTest : public ::testing::Test {
protected:
    // Builds a 3-row chain: seq=1,2,3, all internally consistent.
    // Returns per-row entry_hashes so callers can tamper selectively.
    struct ChainRow {
        int64_t seq;
        int64_t ts_ms;
        std::string actor_user_id;
        std::string actor_kind;
        std::string action;
        std::string outcome;
        std::string details_json;
        std::vector<std::byte> prev_hash;
        std::vector<std::byte> entry_hash;
    };

    static std::vector<ChainRow> build_chain(int n_rows = 3) {
        std::vector<ChainRow> rows;
        std::vector<std::byte> prev_hash(32, std::byte{0});
        for (int i = 1; i <= n_rows; ++i) {
            ChainRow r;
            r.seq           = i;
            r.ts_ms         = 1000000LL + i * 1000;
            r.actor_user_id = "user-abc";
            r.actor_kind    = "user";
            r.action        = "test_action_" + std::to_string(i);
            r.outcome       = "success";
            r.details_json  = "{\"seq\":" + std::to_string(i) + "}";
            r.prev_hash     = prev_hash;
            r.entry_hash    = compute_hash_for_row(
                r.seq, r.ts_ms, r.actor_user_id, r.actor_kind,
                /*domain*/"", /*subject_id*/"", /*subject_kind*/"",
                r.action, r.outcome, r.details_json, r.prev_hash);
            prev_hash = r.entry_hash;
            rows.push_back(r);
        }
        return rows;
    }

    static void insert_chain(Database& db, const std::vector<ChainRow>& rows) {
        for (const auto& r : rows) {
            insert_row(db, r.seq, r.ts_ms * INT64_C(1'000'000),
                       r.actor_user_id, r.actor_kind,
                       r.action, r.outcome, r.details_json,
                       r.prev_hash, r.entry_hash);
        }
    }
};

// ---------------------------------------------------------------------------
// Test 1: Clean 3-entry chain → nullopt.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_CleanChain_Returns_Nullopt) {
    auto db = make_test_db();
    auto rows = build_chain(3);
    insert_chain(db, rows);

    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value())
        << "Expected nullopt, got error: kind=" << (result ? result->kind : "?")
        << " detail=" << (result ? result->detail : "?");
}

// ---------------------------------------------------------------------------
// Test 2: Tampered details_json → hash_mismatch detected.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_TamperedDetailsJson_DetectsTamper) {
    auto db = make_test_db();
    auto rows = build_chain(3);
    insert_chain(db, rows);

    // Directly UPDATE the details_json of seq=2 to corrupt it.
    db.exec("UPDATE audit_log SET details_json = '{\"seq\":999}' WHERE seq=2;");

    auto result = verify_chain(db);
    ASSERT_TRUE(result.has_value()) << "Expected error but got nullopt";
    EXPECT_EQ(result->seq, 2);
    EXPECT_EQ(result->kind, "hash_mismatch");
}

// ---------------------------------------------------------------------------
// Test 3: Tampered prev_hash → prev_hash_mismatch detected.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_TamperedPrevHash_DetectsTamper) {
    auto db = make_test_db();
    auto rows = build_chain(3);
    insert_chain(db, rows);

    // Corrupt prev_hash of seq=2 (flip it to zeros).
    std::vector<std::byte> bad_prev(32, std::byte{0xAB});
    {
        auto stmt = db.prepare(
            "UPDATE audit_log SET prev_hash=? WHERE seq=2;");
        sqlite3_bind_blob(stmt.get(), 1, bad_prev.data(),
                          static_cast<int>(bad_prev.size()), SQLITE_STATIC);
        ASSERT_EQ(stmt.step(), SQLITE_DONE);
    }

    auto result = verify_chain(db);
    ASSERT_TRUE(result.has_value()) << "Expected error but got nullopt";
    EXPECT_EQ(result->seq, 2);
    // Could be hash_mismatch (because canonical includes prev_hash) or prev_hash_mismatch.
    // The verifier checks hash first, so we expect hash_mismatch since the
    // stored entry_hash was computed over the original prev_hash but we also
    // check prev_hash vs expected_prev_hash; either detection is valid.
    EXPECT_TRUE(result->kind == "hash_mismatch" || result->kind == "prev_hash_mismatch")
        << "Unexpected kind: " << result->kind;
}

// ---------------------------------------------------------------------------
// Test 4: Missing seq=2 → gap detected.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_MissingSeq_DetectsGap) {
    auto db = make_test_db();
    auto rows = build_chain(3);
    insert_chain(db, rows);

    db.exec("DELETE FROM audit_log WHERE seq=2;");

    auto result = verify_chain(db);
    ASSERT_TRUE(result.has_value()) << "Expected error but got nullopt";
    EXPECT_EQ(result->kind, "gap");
    EXPECT_EQ(result->seq, 2);
}

// ---------------------------------------------------------------------------
// Test 5: Duplicate seq — UNIQUE PRIMARY KEY prevents direct INSERT.
// Verify the verifier handles it if somehow it appears.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_DuplicateSeq_DetectsDup) {
    auto db = make_test_db();
    auto rows = build_chain(2);
    insert_chain(db, rows);

    // SQLite PRIMARY KEY prevents duplicate seq INSERT.
    // Test that the constraint itself fires (verifier never sees the dup).
    bool constraint_fired = false;
    try {
        // Attempt to INSERT a duplicate seq=1.
        auto stmt = db.prepare(
            "INSERT INTO audit_log "
            "(seq, ts_unix_nanos, actor_kind, action, outcome, prev_hash, entry_hash) "
            "VALUES (1, 1000000, 'system', 'dup_test', 'failure', ?, ?);");
        std::vector<std::byte> dummy(32, std::byte{0});
        sqlite3_bind_blob(stmt.get(), 1, dummy.data(), 32, SQLITE_STATIC);
        sqlite3_bind_blob(stmt.get(), 2, dummy.data(), 32, SQLITE_STATIC);
        int rc = stmt.step();
        // rc == SQLITE_CONSTRAINT means the UNIQUE constraint fired.
        if (rc == SQLITE_CONSTRAINT) {
            constraint_fired = true;
        }
    } catch (...) {
        constraint_fired = true;
    }
    EXPECT_TRUE(constraint_fired)
        << "Expected UNIQUE constraint to prevent duplicate seq INSERT";

    // The chain without the dup should still be valid.
    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value())
        << "Expected nullopt for clean chain: kind=" << (result ? result->kind : "?");
}

// ---------------------------------------------------------------------------
// Test 6: Empty chain → nullopt.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_EmptyChain_Returns_Nullopt) {
    auto db = make_test_db();
    // No rows inserted.
    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value()) << "Empty chain should return nullopt";
}

// ---------------------------------------------------------------------------
// Test 7: Single-row chain with prev_hash = zeros → nullopt.
// ---------------------------------------------------------------------------
TEST_F(AuditReplayTest, Verify_SingleRowChain_Returns_Nullopt) {
    auto db = make_test_db();
    auto rows = build_chain(1);
    insert_chain(db, rows);

    auto result = verify_chain(db);
    EXPECT_FALSE(result.has_value())
        << "Single-row chain should return nullopt: kind=" << (result ? result->kind : "?");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
