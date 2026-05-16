#include <gtest/gtest.h>

#include "../server/plaid/PlaidApiClient.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// FakeHttpClient — returns canned responses for Plaid API calls.
// ---------------------------------------------------------------------------
class FakePlaidHttpClient : public IHttpClient {
public:
    std::optional<HttpResponse> next_response;
    std::string last_request_url;
    std::string last_request_body;

    std::optional<HttpResponse> send(const HttpRequest& req) override {
        last_request_url = req.url;
        last_request_body = req.body.value_or("");
        return next_response;
    }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class PlaidApiClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        SetEnv("PLAID_CLIENT_ID", "test_client_id");
        SetEnv("PLAID_SECRET", "test_secret");
        SetEnv("PLAID_ENV", "sandbox");
        fake_http_ = std::make_unique<FakePlaidHttpClient>();
    }

    void TearDown() override {
        UnsetEnv("PLAID_CLIENT_ID");
        UnsetEnv("PLAID_SECRET");
        UnsetEnv("PLAID_ENV");
    }

    static void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    static void UnsetEnv(const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    std::unique_ptr<FakePlaidHttpClient> fake_http_;
};

// ---------------------------------------------------------------------------
// LinkTokenCreate_Success
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, LinkTokenCreate_Success) {
    json resp_body = {{"link_token", "link-sandbox-abc123"}, {"expiration", "2026-12-31T23:59:59Z"}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.link_token_create("user_entity_1");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "link-sandbox-abc123");
    EXPECT_TRUE(fake_http_->last_request_url.find("/link/token/create") != std::string::npos);

    json sent = json::parse(fake_http_->last_request_body);
    EXPECT_EQ(sent["client_user_id"], "user_entity_1");
    EXPECT_EQ(sent["client_id"], "test_client_id");
}

// ---------------------------------------------------------------------------
// LinkTokenCreate_ApiError_ReturnsNullopt
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, LinkTokenCreate_ApiError_ReturnsNullopt) {
    json resp_body = {{"error_code", "INVALID_INPUT"}, {"error_message", "bad request"}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.link_token_create("user_entity_1");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// LinkTokenCreate_TransportError_ReturnsNullopt
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, LinkTokenCreate_TransportError_ReturnsNullopt) {
    fake_http_->next_response = std::nullopt;

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.link_token_create("user_entity_1");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// ItemPublicTokenExchange_Success
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemPublicTokenExchange_Success) {
    json resp_body = {{"access_token", "access-sandbox-xyz789"}, {"item_id", "item_abc"}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.item_public_token_exchange("public-sandbox-test-token");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "access-sandbox-xyz789");
    EXPECT_TRUE(fake_http_->last_request_url.find("/item/public_token/exchange") != std::string::npos);

    json sent = json::parse(fake_http_->last_request_body);
    EXPECT_EQ(sent["public_token"], "public-sandbox-test-token");
}

// ---------------------------------------------------------------------------
// ItemPublicTokenExchange_ApiError_ReturnsNullopt
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemPublicTokenExchange_ApiError_ReturnsNullopt) {
    json resp_body = {{"error_code", "ITEM_LOGIN_REQUIRED"}, {"error_message", "login required"}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.item_public_token_exchange("public-sandbox-test-token");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// ItemPublicTokenExchange_Non2xx_ReturnsNullopt
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemPublicTokenExchange_Non2xx_ReturnsNullopt) {
    json resp_body = {{"error_code", "RATE_LIMIT"}};
    fake_http_->next_response = HttpResponse{429, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    auto result = client.item_public_token_exchange("public-sandbox-test-token");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// ItemRemove_Success
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemRemove_Success) {
    json resp_body = {{"removed", true}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    bool result = client.item_remove("access-sandbox-some-token");

    EXPECT_TRUE(result);
    EXPECT_TRUE(fake_http_->last_request_url.find("/item/remove") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ItemRemove_NotRemoved_ReturnsFalse
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemRemove_NotRemoved_ReturnsFalse) {
    json resp_body = {{"removed", false}};
    fake_http_->next_response = HttpResponse{200, {}, resp_body.dump()};

    tf::plaid::PlaidApiClient client(*fake_http_);
    bool result = client.item_remove("access-sandbox-some-token");
    EXPECT_FALSE(result);
}

// ---------------------------------------------------------------------------
// ItemRemove_TransportError_ReturnsFalse
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, ItemRemove_TransportError_ReturnsFalse) {
    fake_http_->next_response = std::nullopt;

    tf::plaid::PlaidApiClient client(*fake_http_);
    bool result = client.item_remove("access-sandbox-some-token");
    EXPECT_FALSE(result);
}

// ---------------------------------------------------------------------------
// CredentialsAvailable_TrueWhenSet
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, CredentialsAvailable_TrueWhenSet) {
    tf::plaid::PlaidApiClient client(*fake_http_);
    EXPECT_TRUE(client.credentials_available());
}

// ---------------------------------------------------------------------------
// CredentialsAvailable_FalseWhenUnset
// ---------------------------------------------------------------------------
TEST_F(PlaidApiClientTest, CredentialsAvailable_FalseWhenUnset) {
    UnsetEnv("PLAID_CLIENT_ID");
    UnsetEnv("PLAID_SECRET");
    tf::plaid::PlaidApiClient client(*fake_http_);
    EXPECT_FALSE(client.credentials_available());
}
