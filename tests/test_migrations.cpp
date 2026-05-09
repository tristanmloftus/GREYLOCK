// tests/test_migrations.cpp
//
// GoogleTest suite for the Migrations framework and M001_initial_schema.
//
// All tests use an in-memory SQLite database (:memory:) so they leave no files
// on disk and run in any working directory.

#include "server/db/Database.h"
#include "server/db/Migrations.h"

#include <sqlite3.h>

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns true if a table with the given name exists in sqlite_master.
static bool table_exists(Database& db, const std::string& table_name) {
    auto stmt = db.prepare(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name=?;");
    sqlite3_bind_text(stmt.get(), 1, table_name.c_str(),
                      static_cast<int>(table_name.size()), SQLITE_STATIC);
    int rc = stmt.step();
    if (rc != SQLITE_ROW) return false;
    return sqlite3_column_int(stmt.get(), 0) > 0;
}

// Returns true if an index with the given name exists in sqlite_master.
static bool index_exists(Database& db, const std::string& index_name) {
    auto stmt = db.prepare(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='index' AND name=?;");
    sqlite3_bind_text(stmt.get(), 1, index_name.c_str(),
                      static_cast<int>(index_name.size()), SQLITE_STATIC);
    int rc = stmt.step();
    if (rc != SQLITE_ROW) return false;
    return sqlite3_column_int(stmt.get(), 0) > 0;
}

// Returns number of rows in schema_migrations for the given version.
static int schema_migration_count(Database& db, int version) {
    auto stmt = db.prepare(
        "SELECT COUNT(*) FROM schema_migrations WHERE version=?;");
    sqlite3_bind_int(stmt.get(), 1, version);
    int rc = stmt.step();
    if (rc != SQLITE_ROW) return -1;
    return sqlite3_column_int(stmt.get(), 0);
}

// ---------------------------------------------------------------------------
// Fixture: creates a fresh in-memory Database for each test.
// ---------------------------------------------------------------------------
class MigrationsTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<Database>(":memory:");
    }
    void TearDown() override {
        db_.reset();
    }

    // Convenience: build a Migrations runner that already has M001 registered.
    static Migrations make_runner_with_m001() {
        Migrations m;
        m.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
        return m;
    }

    std::unique_ptr<Database> db_;
};

// ---------------------------------------------------------------------------
// Test 1: Fresh DB applies all pending migrations, current_version returns 1.
// ---------------------------------------------------------------------------
TEST_F(MigrationsTest, Migrations_FreshDbAppliesAllPendingThenAtCurrentVersion) {
    Migrations runner = make_runner_with_m001();

    // Should not throw.
    ASSERT_NO_THROW(runner.apply_pending(*db_));

    // current_version should be 1.
    EXPECT_EQ(runner.current_version(*db_), 1);

    // schema_migrations should have one row for version 1.
    EXPECT_EQ(schema_migration_count(*db_, 1), 1);
}

// ---------------------------------------------------------------------------
// Test 2: Applying migrations twice is idempotent — no errors, no duplicate rows.
// ---------------------------------------------------------------------------
TEST_F(MigrationsTest, Migrations_IdempotentOnReapply) {
    Migrations runner = make_runner_with_m001();

    ASSERT_NO_THROW(runner.apply_pending(*db_));
    // Apply again — should be a no-op.
    ASSERT_NO_THROW(runner.apply_pending(*db_));

    // Still at version 1; no duplicate rows.
    EXPECT_EQ(runner.current_version(*db_), 1);
    EXPECT_EQ(schema_migration_count(*db_, 1), 1);
}

// ---------------------------------------------------------------------------
// Test 3: Transaction rollback on error.
//
// Register M001 plus a M999 whose up() throws after creating a table.
// Assert that M999's table is NOT present (transaction was rolled back)
// and current_version is still 1 (M999 not recorded).
// ---------------------------------------------------------------------------
TEST_F(MigrationsTest, Migrations_TransactionRollbackOnError) {
    Migrations runner = make_runner_with_m001();

    // M999: creates a sentinel table, then throws.
    runner.register_migration({999, "M999_failing", [](Database& db) {
        db.exec("CREATE TABLE migration_rollback_sentinel (id TEXT PRIMARY KEY);");
        throw std::runtime_error("intentional migration failure for test");
    }});

    // apply_pending should throw because M999 fails.
    ASSERT_THROW(runner.apply_pending(*db_), std::runtime_error);

    // M001 was applied before M999, so version is 1.
    EXPECT_EQ(runner.current_version(*db_), 1);

    // M999 was rolled back, so the sentinel table must NOT exist.
    EXPECT_FALSE(table_exists(*db_, "migration_rollback_sentinel"));

    // M999 must NOT be recorded in schema_migrations.
    EXPECT_EQ(schema_migration_count(*db_, 999), 0);
}

// ---------------------------------------------------------------------------
// Test 4: M001 creates all 8 application tables (plus schema_migrations itself).
// ---------------------------------------------------------------------------
TEST_F(MigrationsTest, Migrations_M001CreatesAll8Tables) {
    Migrations runner = make_runner_with_m001();
    ASSERT_NO_THROW(runner.apply_pending(*db_));

    // The 8 application tables from M001_initial_schema.
    EXPECT_TRUE(table_exists(*db_, "users"));
    EXPECT_TRUE(table_exists(*db_, "entities"));
    EXPECT_TRUE(table_exists(*db_, "entity_memberships"));
    EXPECT_TRUE(table_exists(*db_, "accounts"));
    EXPECT_TRUE(table_exists(*db_, "transactions"));
    EXPECT_TRUE(table_exists(*db_, "audit_log"));
    EXPECT_TRUE(table_exists(*db_, "sessions"));
    EXPECT_TRUE(table_exists(*db_, "enrollment_tokens"));

    // schema_migrations is created by the runner itself (not by M001).
    EXPECT_TRUE(table_exists(*db_, "schema_migrations"));
}

// ---------------------------------------------------------------------------
// Test 5: M001 creates the (account_id, posted_at_unix) index on transactions.
// ---------------------------------------------------------------------------
TEST_F(MigrationsTest, Migrations_M001IndexesPresent) {
    Migrations runner = make_runner_with_m001();
    ASSERT_NO_THROW(runner.apply_pending(*db_));

    // The named index from M001_initial_schema_up.
    EXPECT_TRUE(index_exists(*db_, "idx_transactions_account_posted"));
}
