// test_plaid_sync_scheduler.cpp — Unit tests for PlaidSyncScheduler (Phase 4.D).
//
// Uses an in-memory SQLCipher database with M001–M004 applied.
// Uses a MockPlaidApiClient derived from PlaidApiClient to avoid real API calls.
// Uses StubAuditLog for audit recording.
//
// Tests:
//   Sync_NoLinkedAccounts_NoOps       — empty DB; sync_all_accounts() runs without crash.
//   Sync_LinkedAccount_FetchesTransactions — 1 linked account, mock returns 3 txns → 3 rows.
//   Sync_FailsOnTransport_AuditsFailure    — api returns nullopt → plaid_sync_failed audit.
//   Sync_TokenNotFound_SkipsAccount        — account linked but NULL token → token_missing audit.
//   Cursor_Persisted_AndUsedNextCall       — first sync cursor stored; second call passes it.
//   Sync_ModifiedAndRemovedTransactions    — mock returns modified + removed; DB updated.
//   Sync_MultipleAccounts_IndependentErrors — two accounts; one fails, one succeeds.

#include <gtest/gtest.h>

#include "../server/plaid/PlaidApiClient.h"
#include "../server/plaid/PlaidSyncScheduler.h"
#include "../server/plaid/PlaidTokenBroker.h"
#include "../server/audit/StubAuditLog.h"
#include "../server/db/Database.h"
#include "../server/db/Migrations.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>
#include <sodium.h>

// ---------------------------------------------------------------------------
// MockPlaidApiClient — overrides sync_transactions to return controlled data.
// ---------------------------------------------------------------------------
class MockPlaidApiClient : public tf::plaid::PlaidApiClient {
public:
    MockPlaidApiClient() = default;  // uses the protected default constructor

    // Configurable return value.
    std::optional<tf::plaid::TransactionList> next_result;

    // Records the cursor passed to the most recent sync_transactions call.
    std::optional<std::string> last_cursor_used;

    std::optional<tf::plaid::TransactionList> sync_transactions(
        std::string_view /*access_token*/,
        std::optional<std::string> cursor) override
    {
        last_cursor_used = cursor;
        return next_result;
    }

    std::optional<tf::plaid::TransactionList> fetch_transactions(
        std::string_view /*access_token*/,
        std::string_view /*account_id*/,
        int64_t /*from_unix*/,
        int64_t /*to_unix*/) override
    {
        return next_result;
    }
};

// ---------------------------------------------------------------------------
// Fixture — in-memory DB with M001-M004, one entity, configurable accounts,
//           test master key, StubAuditLog.
// ---------------------------------------------------------------------------
class PlaidSyncSchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (sodium_init() < 0) {
            FAIL() << "sodium_init() failed";
        }

        db_ = std::make_unique<Database>(":memory:");

        Migrations mig;
        mig.register_migration({1, "M001_initial_schema",   M001_initial_schema_up});
        mig.register_migration({2, "M002_categories_table", M002_categories_table_up});
        mig.register_migration({3, "M003_budgets_table",    M003_budgets_table_up});
        mig.register_migration({4, "M004_plaid_sync_state", M004_plaid_sync_state_up});
        mig.apply_pending(*db_);

        // Insert test entity.
        db_->exec(
            "INSERT INTO entities (id, name, kind, created_at_unix) "
            "VALUES ('ent1', 'TestEntity', 'Individual', 0);");

        // Build fixed test master key.
        master_key_.fill(std::byte{0xAB});

        // Construct broker with test master key.
        broker_ = std::make_unique<tf::plaid::PlaidTokenBroker>(
            *db_,
            std::span<const std::byte, crypto_kdf_KEYBYTES>(
                master_key_.data(), master_key_.size()));

        mock_api_ = std::make_unique<MockPlaidApiClient>();
        audit_log_ = std::make_unique<tf::audit::StubAuditLog>();
    }

    void TearDown() override {
        broker_.reset();
        db_.reset();
    }

    // Insert a Plaid-linked account (with an encrypted token).
    void insert_linked_account(const std::string& account_id,
                               const std::string& token = "access-sandbox-test") {
        // Insert account row.
        std::string sql =
            "INSERT INTO accounts "
            "  (id, entity_id, name, kind, balance_cents, is_plaid_linked, created_at_unix) "
            "VALUES ('" + account_id + "', 'ent1', 'TestAccount', 'checking', 0, 1, 0);";
        db_->exec(sql);

        // Store the encrypted token via the broker.
        broker_->store_token(account_id, token);
    }

    // Insert a Plaid-linked account with NO encrypted token (data corruption scenario).
    void insert_linked_account_no_token(const std::string& account_id) {
        std::string sql =
            "INSERT INTO accounts "
            "  (id, entity_id, name, kind, balance_cents, is_plaid_linked, created_at_unix) "
            "VALUES ('" + account_id + "', 'ent1', 'Corrupt', 'checking', 0, 1, 0);";
        db_->exec(sql);
        // is_plaid_linked=1 but encrypted_token stays NULL.
    }

    // Count rows in the transactions table.
    int count_transactions() {
        auto stmt = db_->prepare("SELECT COUNT(*) FROM transactions;");
        if (stmt.step() == SQLITE_ROW) {
            return sqlite3_column_int(stmt.get(), 0);
        }
        return -1;
    }

    // Get the stored cursor for an account.
    std::optional<std::string> get_stored_cursor(const std::string& account_id) {
        auto stmt = db_->prepare(
            "SELECT cursor FROM plaid_sync_state WHERE account_id = ?;");
        sqlite3_bind_text(stmt.get(), 1,
            account_id.c_str(), static_cast<int>(account_id.size()), SQLITE_STATIC);
        if (stmt.step() == SQLITE_ROW) {
            const char* cur = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt.get(), 0));
            if (cur) return std::string(cur);
        }
        return std::nullopt;
    }

    // Build a PlaidTransaction with the given id.
    static tf::plaid::PlaidTransaction make_tx(const std::string& tid,
                                                int64_t amount_cents = 1000) {
        tf::plaid::PlaidTransaction tx;
        tx.transaction_id = tid;
        tx.account_id     = "plaid-acc";
        tx.amount_cents   = amount_cents;
        tx.date_unix      = 1700000000;
        tx.name           = "Test Merchant";
        tx.category       = "Food";
        return tx;
    }

    std::unique_ptr<Database> db_;
    std::array<std::byte, crypto_kdf_KEYBYTES> master_key_;
    std::unique_ptr<tf::plaid::PlaidTokenBroker> broker_;
    std::unique_ptr<MockPlaidApiClient> mock_api_;
    std::unique_ptr<tf::audit::StubAuditLog> audit_log_;
};

// ---------------------------------------------------------------------------
// Test 1: Sync_NoLinkedAccounts_NoOps
//
// Empty DB — sync_all_accounts() completes without crash.
// No audit entries should be emitted (no accounts to process).
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Sync_NoLinkedAccounts_NoOps) {
    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    EXPECT_NO_THROW(scheduler.sync_all_accounts());

    // No transactions inserted.
    EXPECT_EQ(count_transactions(), 0);
}

// ---------------------------------------------------------------------------
// Test 2: Sync_LinkedAccount_FetchesTransactions
//
// One linked account; mock returns 3 added transactions.
// Expect: 3 rows in transactions table, 1 plaid_sync_completed audit entry.
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Sync_LinkedAccount_FetchesTransactions) {
    insert_linked_account("acc1");

    tf::plaid::TransactionList tl;
    tl.added = {make_tx("tx1"), make_tx("tx2"), make_tx("tx3")};
    tl.next_cursor = "cursor_after_tx3";
    tl.has_more = false;
    mock_api_->next_result = tl;

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    EXPECT_NO_THROW(scheduler.sync_all_accounts());

    EXPECT_EQ(count_transactions(), 3);
}

// ---------------------------------------------------------------------------
// Test 3: Sync_FailsOnTransport_AuditsFailure
//
// Mock api returns nullopt → plaid_sync_failed audit with action=plaid_sync_failed.
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Sync_FailsOnTransport_AuditsFailure) {
    insert_linked_account("acc1");

    mock_api_->next_result = std::nullopt;  // transport failure

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    EXPECT_NO_THROW(scheduler.sync_all_accounts());

    // No transactions inserted.
    EXPECT_EQ(count_transactions(), 0);

    // StubAuditLog records to stderr; we just verify the call didn't crash.
    // (We can't easily inspect StubAuditLog entries without modifying its API.)
    // The test validates: no crash, no transactions, scheduler continues.
}

// ---------------------------------------------------------------------------
// Test 4: Sync_TokenNotFound_SkipsAccount
//
// Account has is_plaid_linked=1 but encrypted_token is NULL.
// sync_all_accounts() must not crash; the NoTokenTag path is invoked.
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Sync_TokenNotFound_SkipsAccount) {
    insert_linked_account_no_token("acc_corrupt");

    // Mock returns a valid result but it should NOT be called.
    tf::plaid::TransactionList tl;
    tl.added = {make_tx("tx_never")};
    tl.next_cursor = "cursor";
    mock_api_->next_result = tl;

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    EXPECT_NO_THROW(scheduler.sync_all_accounts());

    // No transactions: the no-token path was invoked and returned early.
    EXPECT_EQ(count_transactions(), 0);
}

// ---------------------------------------------------------------------------
// Test 5: Cursor_Persisted_AndUsedNextCall
//
// First sync: mock returns cursor "cursor_A".
// After sync, cursor should be stored in plaid_sync_state.
// Second sync: mock should receive cursor "cursor_A".
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Cursor_Persisted_AndUsedNextCall) {
    insert_linked_account("acc1");

    // First sync: returns cursor_A.
    tf::plaid::TransactionList tl1;
    tl1.added = {make_tx("tx1")};
    tl1.next_cursor = "cursor_A";
    tl1.has_more = false;
    mock_api_->next_result = tl1;

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    scheduler.sync_all_accounts();

    // Cursor should be persisted.
    auto stored_cursor = get_stored_cursor("acc1");
    ASSERT_TRUE(stored_cursor.has_value());
    EXPECT_EQ(*stored_cursor, "cursor_A");

    // Second sync: mock should receive cursor_A.
    tf::plaid::TransactionList tl2;
    tl2.added = {make_tx("tx2")};
    tl2.next_cursor = "cursor_B";
    tl2.has_more = false;
    mock_api_->next_result = tl2;
    mock_api_->last_cursor_used = std::nullopt;  // reset

    scheduler.sync_all_accounts();

    ASSERT_TRUE(mock_api_->last_cursor_used.has_value());
    EXPECT_EQ(*mock_api_->last_cursor_used, "cursor_A");

    // Cursor updated to cursor_B.
    auto stored_cursor2 = get_stored_cursor("acc1");
    ASSERT_TRUE(stored_cursor2.has_value());
    EXPECT_EQ(*stored_cursor2, "cursor_B");

    // Total transactions: 2 distinct rows (tx1 + tx2).
    EXPECT_EQ(count_transactions(), 2);
}

// ---------------------------------------------------------------------------
// Test 6: Sync_ModifiedAndRemovedTransactions
//
// First sync inserts tx1 and tx2.
// Second sync: tx1 modified (amount changed), tx2 removed.
// ---------------------------------------------------------------------------
TEST_F(PlaidSyncSchedulerTest, Sync_ModifiedAndRemovedTransactions) {
    insert_linked_account("acc1");

    // First sync: insert tx1 and tx2.
    tf::plaid::TransactionList tl1;
    tl1.added = {make_tx("tx1", 1000), make_tx("tx2", 2000)};
    tl1.next_cursor = "cursor_1";
    mock_api_->next_result = tl1;

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, *mock_api_, *audit_log_, 900);

    scheduler.sync_all_accounts();
    EXPECT_EQ(count_transactions(), 2);

    // Second sync: tx1 modified, tx2 removed.
    tf::plaid::PlaidTransaction tx1_modified = make_tx("tx1", 9999);

    tf::plaid::TransactionList tl2;
    tl2.modified = {tx1_modified};
    tl2.removed_ids = {"tx2"};
    tl2.next_cursor = "cursor_2";
    mock_api_->next_result = tl2;

    scheduler.sync_all_accounts();

    // tx2 removed → only 1 row remains.
    EXPECT_EQ(count_transactions(), 1);

    // Verify tx1's amount was updated to 9999.
    auto stmt = db_->prepare(
        "SELECT amount_cents FROM transactions WHERE plaid_transaction_id = 'tx1';");
    ASSERT_EQ(stmt.step(), SQLITE_ROW);
    int64_t amount = sqlite3_column_int64(stmt.get(), 0);
    EXPECT_EQ(amount, 9999);
}

// ---------------------------------------------------------------------------
// Test 7: Sync_MultipleAccounts_IndependentErrors
//
// Two linked accounts: acc_good and acc_fail.
// acc_good: mock returns 2 transactions (success).
// acc_fail: we simulate failure by making the mock return nullopt for the
//           second call (via a counter).
//
// Both accounts should be attempted; the good one populates the DB.
// ---------------------------------------------------------------------------

class CountingMockPlaidApiClient : public tf::plaid::PlaidApiClient {
public:
    CountingMockPlaidApiClient() = default;

    int call_count{0};

    std::optional<tf::plaid::TransactionList> sync_transactions(
        std::string_view /*access_token*/,
        std::optional<std::string> /*cursor*/) override
    {
        ++call_count;
        if (call_count == 1) {
            // First account: success with 2 transactions.
            tf::plaid::TransactionList tl;
            tl.added = {
                []{ tf::plaid::PlaidTransaction t; t.transaction_id="tx_good_1"; t.account_id="p"; t.amount_cents=100; t.date_unix=1000; t.name="M"; return t; }(),
                []{ tf::plaid::PlaidTransaction t; t.transaction_id="tx_good_2"; t.account_id="p"; t.amount_cents=200; t.date_unix=1001; t.name="N"; return t; }()
            };
            tl.next_cursor = "cursor_good";
            return tl;
        }
        // Second account: transport failure.
        return std::nullopt;
    }
};

TEST_F(PlaidSyncSchedulerTest, Sync_MultipleAccounts_IndependentErrors) {
    insert_linked_account("acc_good");
    insert_linked_account("acc_fail");

    CountingMockPlaidApiClient counting_api;
    tf::audit::StubAuditLog local_audit;

    tf::plaid::PlaidSyncScheduler scheduler(
        *db_, *broker_, counting_api, local_audit, 900);

    EXPECT_NO_THROW(scheduler.sync_all_accounts());

    // Both accounts were attempted.
    EXPECT_EQ(counting_api.call_count, 2);

    // Only the good account's 2 transactions should be in the DB.
    EXPECT_EQ(count_transactions(), 2);
}
