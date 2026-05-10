// tests/test_v01_migrator.cpp
//
// Unit tests for V01Migrator (Phase 4.F).
//
// All tests use FakeBackendClient — no network required.
//
// Test cases (7 total):
//   1.  Migrate_EmptyJson_NoOps
//   2.  Migrate_OneEntity_CreatesIt
//   3.  Migrate_DuplicateEntity_Skips
//   4.  Migrate_TransactionWithoutAccount_Errors
//   5.  Migrate_LargeJson_StreamsProgress
//   6.  Migrate_PlaidAccessToken_NotSent
//   7.  Migrate_MultipleErrors_AllRecorded

#include <gtest/gtest.h>

#include "../src/migration/V01Migrator.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// --------------------------------------------------------------------------
// FakeBackendClient
//
// Per-path response map: the key is the POST path.  If a path is present in
// `responses`, the next response in the queue is returned.  If the queue is
// exhausted (or the path is not present), a generic 201-created is returned.
//
// Also records every outgoing request for assertion.
// --------------------------------------------------------------------------

struct CapturedRequest {
    std::string path;
    json        body;
    std::string session_token;
};

class FakeBackendClient : public IBackendClient {
public:
    // Queue a response for a specific path.
    // First-in, first-out per path.
    void enqueue(const std::string& path,
                 std::variant<json, MigrationBackendError> resp)
    {
        queues_[path].push_back(std::move(resp));
    }

    // Enqueue a 409 Conflict for a path.
    void enqueue_conflict(const std::string& path) {
        enqueue(path, MigrationBackendError{
            MigrationBackendError::Kind::Conflict, 409, "conflict", "already exists"
        });
    }

    // Enqueue a 404 Not Found for a path.
    void enqueue_not_found(const std::string& path) {
        enqueue(path, MigrationBackendError{
            MigrationBackendError::Kind::NotFound, 404, "not_found", "not found"
        });
    }

    // Default response for unqueued paths: 201 Created with an empty JSON object.
    std::variant<json, MigrationBackendError> default_response = json::object();

    // All requests sent via post_migration().
    std::vector<CapturedRequest> requests;

    std::variant<json, MigrationBackendError> post_migration(
        const std::string& path,
        const json& body,
        const std::string& session_token) override
    {
        requests.push_back({path, body, session_token});

        auto it = queues_.find(path);
        if (it != queues_.end() && !it->second.empty()) {
            auto resp = std::move(it->second.front());
            it->second.erase(it->second.begin());
            return resp;
        }
        return default_response;
    }

    // Convenience: find the last request matching a path prefix.
    std::optional<CapturedRequest> last_request_for(const std::string& path_prefix) const {
        for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
            if (it->path.rfind(path_prefix, 0) == 0) {
                return *it;
            }
        }
        return std::nullopt;
    }

private:
    std::map<std::string, std::vector<std::variant<json, MigrationBackendError>>> queues_;
};

// --------------------------------------------------------------------------
// Test fixture: writes JSON to a temp file; cleans up on teardown.
// --------------------------------------------------------------------------

class V01MigratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / ("v01migrator_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    fs::path write_json(const std::string& filename, const json& j) {
        fs::path p = tmp_dir_ / filename;
        std::ofstream f(p);
        f << j.dump(2);
        return p;
    }

    fs::path tmp_dir_;
};

// --------------------------------------------------------------------------
// 1. Migrate_EmptyJson_NoOps
//    JSON with no collections; report all zeros; no requests sent.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_EmptyJson_NoOps) {
    auto path = write_json("empty.json", json::object());

    FakeBackendClient fake;
    V01Migrator migrator(fake, "session_token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.entities_created, 0);
    EXPECT_EQ(report.entities_skipped, 0);
    EXPECT_EQ(report.accounts_created, 0);
    EXPECT_EQ(report.accounts_skipped, 0);
    EXPECT_EQ(report.transactions_created, 0);
    EXPECT_EQ(report.transactions_skipped, 0);
    EXPECT_EQ(report.categories_created, 0);
    EXPECT_EQ(report.categories_skipped, 0);
    EXPECT_EQ(report.budgets_created, 0);
    EXPECT_EQ(report.budgets_skipped, 0);
    EXPECT_EQ(report.errors, 0);
    EXPECT_TRUE(report.error_messages.empty());
    EXPECT_TRUE(fake.requests.empty());
}

// --------------------------------------------------------------------------
// 2. Migrate_OneEntity_CreatesIt
//    JSON with one entity; FakeBackendClient returns 201; entities_created=1.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_OneEntity_CreatesIt) {
    json j;
    j["entities"] = json::array({
        {{"id", "ent_001"}, {"name", "Personal"}, {"type", "individual"},
         {"tax_id", "123-45-6789"}, {"is_active", true}, {"created_at", "2026-01-01"}}
    });

    auto path = write_json("one_entity.json", j);

    FakeBackendClient fake;
    // Default response is 201 created — no need to enqueue anything.
    V01Migrator migrator(fake, "my_token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.entities_created, 1);
    EXPECT_EQ(report.entities_skipped, 0);
    EXPECT_EQ(report.errors, 0);
    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].path, "/entities");
    EXPECT_EQ(fake.requests[0].body["id"], "ent_001");
    EXPECT_EQ(fake.requests[0].session_token, "my_token");
}

// --------------------------------------------------------------------------
// 3. Migrate_DuplicateEntity_Skips
//    Server returns 409; entities_skipped=1; not counted as error.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_DuplicateEntity_Skips) {
    json j;
    j["entities"] = json::array({
        {{"id", "ent_dup"}, {"name", "Duplicate"}, {"type", "individual"}}
    });

    auto path = write_json("duplicate.json", j);

    FakeBackendClient fake;
    fake.enqueue_conflict("/entities");
    V01Migrator migrator(fake, "token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.entities_created, 0);
    EXPECT_EQ(report.entities_skipped, 1);
    EXPECT_EQ(report.errors, 0);
    EXPECT_TRUE(report.error_messages.empty());
}

// --------------------------------------------------------------------------
// 4. Migrate_TransactionWithoutAccount_Errors
//    Server returns 404 for /accounts/<id>/transactions; errors+=1 with message.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_TransactionWithoutAccount_Errors) {
    json j;
    j["transactions"] = json::array({
        {{"id", "tx_001"}, {"account_id", "acc_missing"},
         {"date", "2026-01-15"}, {"amount", -50.0},
         {"description", "Coffee"}, {"category_id", "cat_food"},
         {"pending", false}, {"plaid_transaction_id", ""},
         {"notes", ""}, {"check_number", ""}}
    });

    auto path = write_json("missing_account_tx.json", j);

    FakeBackendClient fake;
    fake.enqueue_not_found("/accounts/acc_missing/transactions");
    V01Migrator migrator(fake, "token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.transactions_created, 0);
    EXPECT_EQ(report.transactions_skipped, 0);
    EXPECT_EQ(report.errors, 1);
    ASSERT_FALSE(report.error_messages.empty());
    // Message must mention the account ID.
    EXPECT_NE(report.error_messages[0].find("acc_missing"), std::string::npos);
}

// --------------------------------------------------------------------------
// 5. Migrate_LargeJson_StreamsProgress
//    100 entities + 500 transactions; all migrated without crash.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_LargeJson_StreamsProgress) {
    json j;

    // Build 100 entities.
    json entities = json::array();
    for (int i = 0; i < 100; ++i) {
        entities.push_back({
            {"id", "ent_" + std::to_string(i)},
            {"name", "Entity " + std::to_string(i)},
            {"type", "individual"},
            {"is_active", true}
        });
    }
    j["entities"] = entities;

    // Build 500 transactions (all under a fixed account_id).
    json transactions = json::array();
    for (int i = 0; i < 500; ++i) {
        transactions.push_back({
            {"id", "tx_" + std::to_string(i)},
            {"account_id", "acc_main"},
            {"date", "2026-01-01"},
            {"amount", -static_cast<double>(i + 1)},
            {"description", ""},   // empty to be safe
            {"category_id", "cat_other_expense"},
            {"pending", false},
            {"plaid_transaction_id", ""},
            {"notes", ""},
            {"check_number", ""}
        });
    }
    j["transactions"] = transactions;

    auto path = write_json("large.json", j);

    FakeBackendClient fake;
    V01Migrator migrator(fake, "token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.entities_created, 100);
    EXPECT_EQ(report.entities_skipped, 0);
    EXPECT_EQ(report.transactions_created, 500);
    EXPECT_EQ(report.transactions_skipped, 0);
    EXPECT_EQ(report.errors, 0);

    // 100 entity POSTs + 500 transaction POSTs = 600 total requests.
    EXPECT_EQ(static_cast<int>(fake.requests.size()), 600);
}

// --------------------------------------------------------------------------
// 6. Migrate_PlaidAccessToken_NotSent
//    JSON contains a v0.1 plaid_access_token field on an account.
//    Assert the migrator does NOT include it in the POST body.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_PlaidAccessToken_NotSent) {
    json j;
    j["accounts"] = json::array({
        {
            {"id", "acc_plaid"},
            {"name", "Checking"},
            {"entity_id", "ent_001"},
            {"type", "checking"},
            {"balance", 1234.56},
            {"institution", "Chase"},
            {"plaid_item_id", "item_abc123"},
            {"plaid_access_token", "access-sandbox-SECRET-TOKEN"},  // must NOT be sent
            {"is_active", true}
        }
    });

    auto path = write_json("with_plaid_token.json", j);

    FakeBackendClient fake;
    V01Migrator migrator(fake, "token");

    migrator.migrate(path);

    // Should have sent exactly one account POST.
    ASSERT_EQ(fake.requests.size(), 1u);

    const json& body = fake.requests[0].body;

    // plaid_access_token must be absent from the request body.
    EXPECT_FALSE(body.contains("plaid_access_token"))
        << "plaid_access_token must NOT be sent to the server";

    // plaid_item_id is retained (not sensitive).
    EXPECT_TRUE(body.contains("plaid_item_id"));
    EXPECT_EQ(body["plaid_item_id"], "item_abc123");

    // balance is NOT in the POST body (server derives balances from transactions).
    // If this changes, update the migrator and this assertion.
    EXPECT_FALSE(body.contains("balance"))
        << "balance should not be posted; server derives it";
}

// --------------------------------------------------------------------------
// 7. Migrate_MultipleErrors_AllRecorded
//    Three transactions whose accounts don't exist; all three should be
//    recorded as errors; none should abort the migration of the others.
// --------------------------------------------------------------------------

TEST_F(V01MigratorTest, Migrate_MultipleErrors_AllRecorded) {
    json j;
    j["transactions"] = json::array({
        {{"id", "tx_a"}, {"account_id", "miss_a"}, {"date", "2026-01-01"}, {"amount", -1.0}},
        {{"id", "tx_b"}, {"account_id", "miss_b"}, {"date", "2026-01-02"}, {"amount", -2.0}},
        {{"id", "tx_c"}, {"account_id", "miss_c"}, {"date", "2026-01-03"}, {"amount", -3.0}}
    });

    auto path = write_json("multi_error.json", j);

    FakeBackendClient fake;
    // Return 404 for all three.
    fake.enqueue_not_found("/accounts/miss_a/transactions");
    fake.enqueue_not_found("/accounts/miss_b/transactions");
    fake.enqueue_not_found("/accounts/miss_c/transactions");
    V01Migrator migrator(fake, "token");

    MigrationReport report = migrator.migrate(path);

    EXPECT_EQ(report.transactions_created, 0);
    EXPECT_EQ(report.errors, 3);
    ASSERT_EQ(report.error_messages.size(), 3u);

    // Each message should mention the respective account ID.
    EXPECT_NE(report.error_messages[0].find("miss_a"), std::string::npos);
    EXPECT_NE(report.error_messages[1].find("miss_b"), std::string::npos);
    EXPECT_NE(report.error_messages[2].find("miss_c"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
