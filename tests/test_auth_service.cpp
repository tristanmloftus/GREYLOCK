// tests/test_auth_service.cpp
//
// Unit tests for AuthService (Phase 3.B).
//
// Uses:
//   - FakeHttpClient (inlined from test_backend_client.cpp pattern)
//   - FakeSecretStore (in-process std::unordered_map<string, vector<byte>>)
//
// All 12 test cases (10 original + 1 corrective RC-2 addition + 1 split):
//  1. Login_Success_CachesSessionToken
//  2. Login_401_NoCacheChange
//  3. Login_TransportFailure_NoCacheChange
//  4. Logout_RevokesAndClearsCache
//  5. Logout_TransportFailure_LeavesCache
//  6. Whoami_200_ReturnsUserId
//  7. Whoami_401_ClearsCache
//  8. Whoami_NoCachedSession_ReturnsNullopt
//  9. Enroll_Success_DoesNotCacheSession
// 10. HasCachedSession_TrueWhenSet
// 11. HasCachedSession_FalseWhenAbsent
// 12. Whoami_TransportFailure_LeavesCache  (RC corrective)

#include <gtest/gtest.h>

#include "../src/services/AuthService.h"
#include "../src/services/BackendClient.h"
#include "../src/services/IHttpClient.h"
#include "../src/services/ISecretStore.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using json = nlohmann::json;

// --------------------------------------------------------------------------
// FakeHttpClient
// Records the last outgoing request; returns a programmer-supplied response.
// Returning std::nullopt simulates a transport failure.
// --------------------------------------------------------------------------

class FakeHttpClient : public IHttpClient {
public:
    std::optional<HttpResponse> next_response;
    HttpRequest last_request;
    int call_count{0};

    std::optional<HttpResponse> send(const HttpRequest& req) override {
        last_request = req;
        ++call_count;
        return next_response;
    }
};

// --------------------------------------------------------------------------
// FakeSecretStore
// In-process map-backed secret store for testing.
// --------------------------------------------------------------------------

class FakeSecretStore : public ISecretStore {
public:
    std::unordered_map<std::string, std::vector<std::byte>> store;

    bool put(std::string_view key, std::span<const std::byte> value) override {
        store[std::string(key)] = std::vector<std::byte>(value.begin(), value.end());
        return true;
    }

    std::optional<std::vector<std::byte>> get(std::string_view key) override {
        auto it = store.find(std::string(key));
        if (it == store.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool remove(std::string_view key) override {
        store.erase(std::string(key));
        return true;
    }
};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static std::shared_ptr<FakeHttpClient> make_fake_http() {
    return std::make_shared<FakeHttpClient>();
}

static std::shared_ptr<FakeSecretStore> make_fake_secrets() {
    return std::make_shared<FakeSecretStore>();
}

static HttpResponse make_resp(long status, const std::string& body) {
    HttpResponse r;
    r.status_code = status;
    r.body        = body;
    return r;
}

static const std::string kEmail      = "test@example.com";
static const std::string kCacheKey   = "tf-session-test@example.com";
static const std::string kSessionTok = "session-abc123";
static const std::string kUserId     = "user-42";

// Prepopulate the secret store with a valid session token.
static void seed_session(FakeSecretStore& secrets, const std::string& token = kSessionTok) {
    const auto* ptr = reinterpret_cast<const std::byte*>(token.data());
    std::span<const std::byte> sp{ptr, token.size()};
    secrets.put(kCacheKey, sp);
}

// Read the raw token bytes back as a string.
static std::string read_session(FakeSecretStore& secrets) {
    auto opt = secrets.get(kCacheKey);
    if (!opt.has_value()) return "";
    return std::string(reinterpret_cast<const char*>(opt->data()), opt->size());
}

// --------------------------------------------------------------------------
// Test 1: Login_Success_CachesSessionToken
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Login_Success_CachesSessionToken) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();

    json resp_body;
    resp_body["session_token"]  = kSessionTok;
    resp_body["user_id"]        = kUserId;
    resp_body["expires_at"]     = int64_t{9999999999};
    http->next_response = make_resp(200, resp_body.dump());

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    LoginRequest req{kEmail, "pass123", "000000"};
    auto result = auth.login(req);

    ASSERT_TRUE(std::holds_alternative<LoginResult>(result))
        << "Expected LoginResult on 200";
    const auto& lr = std::get<LoginResult>(result);
    EXPECT_EQ(lr.user_id, kUserId);
    EXPECT_EQ(lr.session_token, kSessionTok);

    // Secret store must have been called with "tf-session-test@example.com".
    ASSERT_TRUE(secrets->store.count(kCacheKey) > 0)
        << "Session token should be cached in secret store";
    EXPECT_EQ(read_session(*secrets), kSessionTok);
}

// --------------------------------------------------------------------------
// Test 2: Login_401_NoCacheChange
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Login_401_NoCacheChange) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();

    http->next_response = make_resp(401, "");

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    LoginRequest req{kEmail, "wrongpass", "000000"};
    auto result = auth.login(req);

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::Unauthorized);

    // Secret store must NOT have been modified.
    EXPECT_TRUE(secrets->store.empty())
        << "Secret store must be unchanged on login failure";
}

// --------------------------------------------------------------------------
// Test 3: Login_TransportFailure_NoCacheChange
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Login_TransportFailure_NoCacheChange) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();

    http->next_response = std::nullopt;  // transport failure

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    LoginRequest req{kEmail, "pass", "123456"};
    auto result = auth.login(req);

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::Transport);

    EXPECT_TRUE(secrets->store.empty())
        << "Secret store must be unchanged on transport failure";
}

// --------------------------------------------------------------------------
// Test 4: Logout_RevokesAndClearsCache
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Logout_RevokesAndClearsCache) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);

    // Server returns 200 OK for logout.
    json resp_body;
    resp_body["ok"] = true;
    http->next_response = make_resp(200, resp_body.dump());

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    bool ok = auth.logout();

    EXPECT_TRUE(ok) << "Logout should return true on successful revocation";
    EXPECT_EQ(http->call_count, 1) << "One HTTP call expected (POST /auth/logout)";

    // Cache must be cleared.
    EXPECT_EQ(secrets->store.count(kCacheKey), 0u)
        << "Session token must be removed from cache after successful logout";
}

// --------------------------------------------------------------------------
// Test 5: Logout_TransportFailure_LeavesCache
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Logout_TransportFailure_LeavesCache) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);

    http->next_response = std::nullopt;  // transport failure

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    bool ok = auth.logout();

    EXPECT_FALSE(ok) << "Logout should return false on transport failure";

    // Cache must be PRESERVED — token may still be valid on the server.
    EXPECT_EQ(secrets->store.count(kCacheKey), 1u)
        << "Session token must NOT be removed from cache on transport failure";
    EXPECT_EQ(read_session(*secrets), kSessionTok);
}

// --------------------------------------------------------------------------
// Test 6: Whoami_200_ReturnsUserId
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Whoami_200_ReturnsUserId) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);

    json resp_body;
    resp_body["user_id"] = kUserId;
    http->next_response = make_resp(200, resp_body.dump());

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    auto user_id = auth.current_user_id();

    ASSERT_TRUE(user_id.has_value()) << "current_user_id should return user_id on 200";
    EXPECT_EQ(*user_id, kUserId);
}

// --------------------------------------------------------------------------
// Test 7: Whoami_401_ClearsCache
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Whoami_401_ClearsCache) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);  // Start with a cached (stale) token.

    http->next_response = make_resp(401, "");

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    auto user_id = auth.current_user_id();

    EXPECT_FALSE(user_id.has_value()) << "current_user_id should return nullopt on 401";

    // Cache must be cleared — token is stale.
    EXPECT_EQ(secrets->store.count(kCacheKey), 0u)
        << "Stale session token must be removed from cache on 401";
}

// --------------------------------------------------------------------------
// Test 8: Whoami_NoCachedSession_ReturnsNullopt
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Whoami_NoCachedSession_ReturnsNullopt) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    // No session seeded — secret store is empty.

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    auto user_id = auth.current_user_id();

    EXPECT_FALSE(user_id.has_value()) << "Should return nullopt with no cached session";
    EXPECT_EQ(http->call_count, 0) << "No HTTP call should be made with no cached session";
}

// --------------------------------------------------------------------------
// Test 9: Enroll_Success_DoesNotCacheSession
// --------------------------------------------------------------------------
TEST(AuthServiceTest, Enroll_Success_DoesNotCacheSession) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();

    json resp_body;
    resp_body["user_id"]               = kUserId;
    resp_body["totp_provisioning_uri"] = "otpauth://totp/test?secret=ABCDEF";
    http->next_response = make_resp(200, resp_body.dump());

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    EnrollRequest req;
    req.token     = "invite-token-xyz";
    req.email     = kEmail;
    req.passphrase = "newpass";
    auto result = auth.enroll(req);

    ASSERT_TRUE(std::holds_alternative<EnrollResult>(result))
        << "Expected EnrollResult on 200";
    const auto& er = std::get<EnrollResult>(result);
    EXPECT_EQ(er.user_id, kUserId);
    EXPECT_FALSE(er.totp_provisioning_uri.empty());

    // Enroll must NOT touch the secret store.
    EXPECT_TRUE(secrets->store.empty())
        << "Enroll must not cache any session token";
}

// --------------------------------------------------------------------------
// Test 10: HasCachedSession_TrueWhenSet / HasCachedSession_FalseWhenAbsent
// --------------------------------------------------------------------------
TEST(AuthServiceTest, HasCachedSession_TrueWhenSet) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    EXPECT_TRUE(auth.has_cached_session())
        << "has_cached_session should return true when token is in secret store";
    EXPECT_EQ(http->call_count, 0) << "has_cached_session must not make network calls";
}

TEST(AuthServiceTest, HasCachedSession_FalseWhenAbsent) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    // No session seeded.

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    EXPECT_FALSE(auth.has_cached_session())
        << "has_cached_session should return false when no token is cached";
    EXPECT_EQ(http->call_count, 0) << "has_cached_session must not make network calls";
}

// --------------------------------------------------------------------------
// Test 11: Whoami_TransportFailure_LeavesCache
// --------------------------------------------------------------------------
// Pre-populate cache with a valid-looking session token.
// Make the FakeHttpClient return std::nullopt (transport failure).
// Assert: returns std::nullopt AND the cache entry is STILL there —
// only 401 should evict the cached token; transport failures must preserve it.
TEST(AuthServiceTest, Whoami_TransportFailure_LeavesCache) {
    auto http    = make_fake_http();
    auto secrets = make_fake_secrets();
    seed_session(*secrets);  // Pre-populate cache.

    http->next_response = std::nullopt;  // Transport failure.

    auto backend = std::make_shared<BackendClient>(http, "https://localhost:8443");
    AuthService auth(backend, secrets, kEmail);

    auto user_id = auth.current_user_id();

    EXPECT_FALSE(user_id.has_value())
        << "current_user_id should return nullopt on transport failure";

    // Cache must be PRESERVED — transport failure does not mean token is stale.
    EXPECT_EQ(secrets->store.count(kCacheKey), 1u)
        << "Session token must NOT be evicted from cache on transport failure";
    EXPECT_EQ(read_session(*secrets), kSessionTok)
        << "Cached token value must be unchanged after transport failure";
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
