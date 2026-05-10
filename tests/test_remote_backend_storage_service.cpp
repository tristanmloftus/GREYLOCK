// tests/test_remote_backend_storage_service.cpp — Phase 4.B unit tests.
//
// Tests for RemoteBackendStorageService using FakeHttpClient (no network).
//
// Test cases (6 total):
//   1. Load_EmptyEntities         — /entities returns empty list, load() returns true
//   2. Load_SingleEntityAndAccount — one entity, one account, one tx
//   3. Load_TokenNeverExposedInAccount — plaid_access_token always ""
//   4. Load_TransportFailure      — GET /entities fails → load() returns false
//   5. Load_ServerError           — GET /entities returns 500 → load() returns false
//   6. Save_IsStub                — save() always returns false

#include <gtest/gtest.h>

#include "../src/services/RemoteBackendStorageService.h"
#include "../src/services/BackendClient.h"
#include "../src/services/IHttpClient.h"
#include "../src/models/Entity.h"
#include "../src/models/Account.h"
#include "../src/models/Transaction.h"
#include "../src/models/Category.h"
#include "../src/models/Budget.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

// --------------------------------------------------------------------------
// FakeHttpClient with URL-substring-matched handlers
// --------------------------------------------------------------------------

class FakeHttpClient : public IHttpClient {
public:
    struct Handler {
        std::string url_substring;
        std::function<HttpResponse(const HttpRequest&)> fn;
    };

    std::vector<Handler> handlers;

    // Fallback response when no handler matches.
    HttpResponse fallback_response{200, {}, R"({"items":[]})"};

    // If true, send() returns nullopt (transport failure).
    bool simulate_transport_failure = false;

    std::optional<HttpResponse> send(const HttpRequest& req) override {
        if (simulate_transport_failure) return std::nullopt;
        for (const auto& h : handlers) {
            if (req.url.find(h.url_substring) != std::string::npos) {
                return h.fn(req);
            }
        }
        return fallback_response;
    }
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static HttpResponse json_resp(int status, const json& body) {
    return HttpResponse{static_cast<long>(status), {}, body.dump()};
}

static std::shared_ptr<RemoteBackendStorageService> make_service(
    std::shared_ptr<FakeHttpClient> fake)
{
    auto backend = std::make_shared<BackendClient>(fake, "https://localhost:19845");
    return std::make_shared<RemoteBackendStorageService>(backend, "test-session-token");
}

// --------------------------------------------------------------------------
// Test 1: Load_EmptyEntities
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Load_EmptyEntities) {
    auto fake = std::make_shared<FakeHttpClient>();
    // /entities returns empty items list
    fake->handlers.push_back({"/entities", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});

    auto svc = make_service(fake);
    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->load(entities, accounts, transactions, categories, budgets);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(entities.empty());
    EXPECT_TRUE(accounts.empty());
    EXPECT_TRUE(transactions.empty());
    EXPECT_TRUE(categories.empty());
    EXPECT_TRUE(budgets.empty());
}

// --------------------------------------------------------------------------
// Test 2: Load_SingleEntityAndAccount
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Load_SingleEntityAndAccount) {
    auto fake = std::make_shared<FakeHttpClient>();

    json entity_item = {
        {"id", "ent-1"},
        {"name", "Test Entity"},
        {"kind", "individual"},
        {"created_at_unix", int64_t{1748736000}}
    };

    json account_item = {
        {"id", "acct-1"},
        {"entity_id", "ent-1"},
        {"name", "Checking"},
        {"kind", "checking"},
        {"balance_cents", int64_t{123456}},
        {"plaid_item_id", nullptr},
        {"is_plaid_linked", false},
        {"created_at_unix", int64_t{1748736000}}
    };

    json tx_item = {
        {"id", "tx-1"},
        {"account_id", "acct-1"},
        {"posted_at_unix", int64_t{1748736000}},
        {"amount_cents", int64_t{-500}},
        {"description", "Coffee"},
        {"category", nullptr}
    };

    fake->handlers.push_back({"/entities/ent-1/accounts", [&](const HttpRequest&) {
        return json_resp(200, {{"items", json::array({account_item})}});
    }});
    fake->handlers.push_back({"/entities/ent-1/categories", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});
    fake->handlers.push_back({"/entities/ent-1/budgets", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});
    fake->handlers.push_back({"/accounts/acct-1/transactions", [&](const HttpRequest&) {
        return json_resp(200, {{"items", json::array({tx_item})}});
    }});
    fake->handlers.push_back({"/entities", [&](const HttpRequest&) {
        return json_resp(200, {{"items", json::array({entity_item})}});
    }});

    auto svc = make_service(fake);
    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->load(entities, accounts, transactions, categories, budgets);
    ASSERT_TRUE(ok) << svc->get_last_error();

    ASSERT_EQ(entities.size(), 1u);
    EXPECT_EQ(entities[0].id, "ent-1");
    EXPECT_EQ(entities[0].name, "Test Entity");

    ASSERT_EQ(accounts.size(), 1u);
    EXPECT_EQ(accounts[0].id, "acct-1");
    EXPECT_EQ(accounts[0].entity_id, "ent-1");
    // 123456 cents → 1234.56
    EXPECT_NEAR(accounts[0].balance, 1234.56, 0.001);

    ASSERT_EQ(transactions.size(), 1u);
    EXPECT_EQ(transactions[0].id, "tx-1");
    EXPECT_EQ(transactions[0].description, "Coffee");
    // -500 cents → -5.00
    EXPECT_NEAR(transactions[0].amount, -5.0, 0.001);
}

// --------------------------------------------------------------------------
// Test 3: Load_TokenNeverExposedInAccount
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Load_TokenNeverExposedInAccount) {
    auto fake = std::make_shared<FakeHttpClient>();

    // Server response deliberately includes a field called "encrypted_token"
    // to simulate a server bug. The service must NOT populate plaid_access_token
    // from it.
    json account_item = {
        {"id", "acct-safe"},
        {"entity_id", "ent-safe"},
        {"name", "Safe Account"},
        {"kind", "savings"},
        {"balance_cents", int64_t{0}},
        {"encrypted_token", "THIS_MUST_NOT_APPEAR_IN_ACCOUNT"},
        {"is_plaid_linked", false},
        {"created_at_unix", int64_t{1748736000}}
    };

    json entity_item = {
        {"id", "ent-safe"},
        {"name", "Safe Entity"},
        {"kind", "individual"},
        {"created_at_unix", int64_t{1748736000}}
    };

    fake->handlers.push_back({"/entities/ent-safe/accounts", [&](const HttpRequest&) {
        return json_resp(200, {{"items", json::array({account_item})}});
    }});
    fake->handlers.push_back({"/entities/ent-safe/categories", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});
    fake->handlers.push_back({"/entities/ent-safe/budgets", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});
    fake->handlers.push_back({"/accounts/acct-safe/transactions", [](const HttpRequest&) {
        return json_resp(200, {{"items", json::array()}});
    }});
    fake->handlers.push_back({"/entities", [&](const HttpRequest&) {
        return json_resp(200, {{"items", json::array({entity_item})}});
    }});

    auto svc = make_service(fake);
    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->load(entities, accounts, transactions, categories, budgets);
    ASSERT_TRUE(ok);
    ASSERT_EQ(accounts.size(), 1u);

    // is_plaid_linked reflects the server-side token presence flag.
    // plaid_access_token was removed by 4.C — tokens are managed server-side.
    EXPECT_FALSE(accounts[0].is_plaid_linked)
        << "is_plaid_linked should default to false when not set in server response";
}

// --------------------------------------------------------------------------
// Test 4: Load_TransportFailure
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Load_TransportFailure) {
    auto fake = std::make_shared<FakeHttpClient>();
    fake->simulate_transport_failure = true;

    auto svc = make_service(fake);
    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->load(entities, accounts, transactions, categories, budgets);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(svc->get_last_error().empty());
}

// --------------------------------------------------------------------------
// Test 5: Load_ServerError
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Load_ServerError) {
    auto fake = std::make_shared<FakeHttpClient>();
    // /entities returns 500
    fake->handlers.push_back({"/entities", [](const HttpRequest&) {
        return json_resp(500, {{"error", "internal"}});
    }});

    auto svc = make_service(fake);
    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->load(entities, accounts, transactions, categories, budgets);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(svc->get_last_error().empty());
}

// --------------------------------------------------------------------------
// Test 6: Save_IsStub
// --------------------------------------------------------------------------

TEST(RemoteBackendStorageServiceTests, Save_IsStub) {
    auto fake = std::make_shared<FakeHttpClient>();
    auto svc = make_service(fake);

    std::vector<Entity>      entities;
    std::vector<Account>     accounts;
    std::vector<Transaction> transactions;
    std::vector<Category>    categories;
    std::vector<Budget>      budgets;

    bool ok = svc->save(entities, accounts, transactions, categories, budgets);
    EXPECT_FALSE(ok) << "save() must return false (Phase 5 stub)";
    EXPECT_FALSE(svc->get_last_error().empty())
        << "save() must set a non-empty last_error describing the stub status";
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
