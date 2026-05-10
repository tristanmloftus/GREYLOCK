// tests/test_data_endpoints.cpp — Phase 4.B data endpoint integration tests.
//
// Launches TerminalFinanceServer as a subprocess (port 19845) and drives the
// full entity/account/transaction/category/budget CRUD flows.
//
// POSIX-only (fork+exec). All tests are skipped on Windows.
//
// Test cases:
//   1. EntityCrudAndTransactionFlow   — full CRUD plus nested account + tx
//   2. UnauthenticatedRequests_Return401 — missing/invalid Bearer token
//   3. CrossUserAccess_Returns403     — user B cannot access user A's entity
//   4. CategoriesCrud                 — category create/list/get/update/delete
//   5. BudgetsCrud                    — budget   create/list/get/update/delete

#include <gtest/gtest.h>

#include "../src/services/http/CurlHttpClient.h"
#include "../src/services/IHttpClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <cerrno>
#  include <cstring>
#  include <csignal>
#  include <cstdio>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <sodium.h>
#  include "../server/auth/HmacSha1.h"
#endif

using json = nlohmann::json;
using namespace std::chrono_literals;

// --------------------------------------------------------------------------
// CMake-injected paths
// --------------------------------------------------------------------------

#ifndef TF_FIXTURES_DIR
#  define TF_FIXTURES_DIR "tests/fixtures"
#endif

#ifndef TF_SERVER_BIN
#  define TF_SERVER_BIN "./TerminalFinanceServer"
#endif

static const std::string kTestCert  = TF_FIXTURES_DIR "/test-cert.pem";
static const std::string kTestKey   = TF_FIXTURES_DIR "/test-key.pem";
static const std::string kTestCA    = TF_FIXTURES_DIR "/test-ca.pem";
static const std::string kServerBin = TF_SERVER_BIN;

static constexpr int kDataLivePort = 19845;

// --------------------------------------------------------------------------
// CA-injecting HTTP client
// --------------------------------------------------------------------------

class CaInjectingClient : public IHttpClient {
public:
    explicit CaInjectingClient(const std::string& ca_path)
        : ca_path_(ca_path) {}

    std::optional<HttpResponse> send(const HttpRequest& req) override {
        HttpRequest r = req;
        r.ca_bundle_path = ca_path_;
        return inner_.send(r);
    }

private:
    std::string    ca_path_;
    CurlHttpClient inner_;
};

#ifndef _WIN32

// --------------------------------------------------------------------------
// TOTP helpers (same as test_auth_endpoints.cpp)
// --------------------------------------------------------------------------

static std::vector<uint8_t> base32_decode_de(const std::string& s) {
    static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : s) {
        const char* p = std::strchr(kAlpha, c);
        if (!p) continue;
        buf = (buf << 5) | static_cast<int>(p - kAlpha);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

static std::string extract_secret_b32_de(const std::string& uri) {
    auto pos = uri.find("secret=");
    if (pos == std::string::npos) return "";
    pos += 7;
    auto end = uri.find('&', pos);
    if (end == std::string::npos) end = uri.size();
    return uri.substr(pos, end - pos);
}

static int compute_test_totp_de(const std::vector<uint8_t>& secret, int64_t t_unix) {
    uint64_t counter = static_cast<uint64_t>(t_unix) / 30;
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; --i) {
        counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    auto hmac = tf::auth::hmacsha1::hmac(
        secret.data(), secret.size(),
        counter_bytes, 8);
    uint8_t offset = hmac[19] & 0x0F;
    uint32_t p = ((hmac[offset]     & 0x7F) << 24)
               | ((hmac[offset + 1] & 0xFF) << 16)
               | ((hmac[offset + 2] & 0xFF) <<  8)
               |  (hmac[offset + 3] & 0xFF);
    return static_cast<int>(p % 1000000);
}

// --------------------------------------------------------------------------
// HTTP helpers
// --------------------------------------------------------------------------

static std::optional<std::pair<int, json>> http_json(
    CaInjectingClient& client,
    const std::string& method,
    const std::string& url,
    const std::string& bearer = "",
    const json* body = nullptr)
{
    HttpRequest req;
    req.method = method;
    req.url    = url;
    if (!bearer.empty()) {
        req.headers["Authorization"] = "Bearer " + bearer;
    }
    if (body) {
        req.headers["Content-Type"] = "application/json";
        req.body = body->dump();
    }
    auto resp = client.send(req);
    if (!resp) return std::nullopt;
    try {
        return std::make_pair(static_cast<int>(resp->status_code),
                              json::parse(resp->body));
    } catch (...) {
        return std::make_pair(static_cast<int>(resp->status_code), json{});
    }
}

// --------------------------------------------------------------------------
// DataEndpointsFixture
// --------------------------------------------------------------------------

class DataEndpointsFixture : public ::testing::Test {
protected:
    pid_t server_pid_{-1};
    std::string db_path_;

    void SetUp() override {
        db_path_ = "/tmp/tf_data_test_" + std::to_string(getpid()) + ".db";

        server_pid_ = ::fork();
        ASSERT_NE(server_pid_, -1) << "fork() failed: " << std::strerror(errno);

        if (server_pid_ == 0) {
            ::setenv("TF_SERVER_PORT",      std::to_string(kDataLivePort).c_str(), 1);
            ::setenv("TF_SERVER_CERT_PATH", kTestCert.c_str(), 1);
            ::setenv("TF_SERVER_KEY_PATH",  kTestKey.c_str(),  1);
            ::setenv("TF_SERVER_BIND_ADDR", "127.0.0.1",       1);
            ::setenv("TF_DB_PATH",          db_path_.c_str(),  1);

            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }

            ::execl(kServerBin.c_str(), kServerBin.c_str(), nullptr);
            ::_exit(127);
        }
    }

    void TearDown() override {
        if (server_pid_ > 0) {
            ::kill(server_pid_, SIGTERM);
            int status = 0;
            ::waitpid(server_pid_, &status, 0);
            server_pid_ = -1;
        }
        ::unlink(db_path_.c_str());
    }

    std::string base_url() const {
        return "https://127.0.0.1:" + std::to_string(kDataLivePort);
    }

    bool wait_for_server() {
        CaInjectingClient poller(kTestCA);
        for (int i = 0; i < 100; ++i) {
            HttpRequest req;
            req.method = "GET";
            req.url    = base_url() + "/healthz";
            auto resp  = poller.send(req);
            if (resp && resp->status_code == 200) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    std::string mint_token(const std::string& email) {
        std::string cmd = kServerBin
            + " --mint-enrollment-token " + email
            + " 2>/dev/null";
        ::setenv("TF_DB_PATH", db_path_.c_str(), 1);
        FILE* fp = ::popen(cmd.c_str(), "r");
        if (!fp) return "";
        char buf[256] = {};
        if (::fgets(buf, sizeof(buf), fp) == nullptr) {
            ::pclose(fp);
            return "";
        }
        ::pclose(fp);
        std::string tok(buf);
        while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) {
            tok.pop_back();
        }
        return tok;
    }

    // Enroll a user, return session token. Asserts internally.
    std::string enroll_and_login(CaInjectingClient& client,
                                  const std::string& email,
                                  const std::string& passphrase)
    {
        std::string enroll_token = mint_token(email);
        if (enroll_token.size() != 64u) return "";

        json enroll_body = {{"token", enroll_token}, {"email", email}, {"passphrase", passphrase}};
        auto enroll_resp = http_json(client, "POST", base_url() + "/auth/enroll", "", &enroll_body);
        if (!enroll_resp || enroll_resp->first != 200) return "";

        std::string prov_uri = enroll_resp->second.value("totp_provisioning_uri", "");
        std::string b32 = extract_secret_b32_de(prov_uri);
        if (b32.empty()) return "";
        auto secret_bytes = base32_decode_de(b32);

        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        int totp = compute_test_totp_de(secret_bytes, now);

        json login_body = {{"email", email}, {"passphrase", passphrase}, {"totp_code", totp}};
        auto login_resp = http_json(client, "POST", base_url() + "/auth/login", "", &login_body);
        if (!login_resp || login_resp->first != 200) return "";

        return login_resp->second.value("session_token", "");
    }
};

// --------------------------------------------------------------------------
// Test 1: EntityCrudAndTransactionFlow
// --------------------------------------------------------------------------

TEST_F(DataEndpointsFixture, EntityCrudAndTransactionFlow) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok = enroll_and_login(client, "user1@example.com", "passphrase-abc-123");
    ASSERT_FALSE(tok.empty()) << "enroll_and_login failed";

    // Create entity
    json create_ent = {{"name","Test Entity"},{"kind","Individual"}};
    auto r_ce = http_json(client, "POST", base_url() + "/entities", tok, &create_ent);
    ASSERT_TRUE(r_ce.has_value());
    ASSERT_EQ(r_ce->first, 201) << r_ce->second.dump();
    std::string eid = r_ce->second.value("id", "");
    ASSERT_FALSE(eid.empty());

    // GET /entities
    auto r_le = http_json(client, "GET", base_url() + "/entities", tok);
    ASSERT_TRUE(r_le.has_value());
    ASSERT_EQ(r_le->first, 200);
    ASSERT_TRUE(r_le->second.contains("items"));
    ASSERT_GE(r_le->second["items"].size(), 1u);

    // GET /entities/<id>
    auto r_ge = http_json(client, "GET", base_url() + "/entities/" + eid, tok);
    ASSERT_TRUE(r_ge.has_value());
    ASSERT_EQ(r_ge->first, 200);
    EXPECT_EQ(r_ge->second.value("id", ""), eid);

    // PUT /entities/<id>
    json upd_ent = {{"name","Updated Entity"}};
    auto r_ue = http_json(client, "PUT", base_url() + "/entities/" + eid, tok, &upd_ent);
    ASSERT_TRUE(r_ue.has_value());
    ASSERT_EQ(r_ue->first, 200);
    EXPECT_EQ(r_ue->second.value("name", ""), "Updated Entity");

    // Create account under entity
    json create_acc = {{"name","Checking"},{"kind","checking"},{"balance_cents",100000}};
    auto r_ca = http_json(client, "POST",
        base_url() + "/entities/" + eid + "/accounts", tok, &create_acc);
    ASSERT_TRUE(r_ca.has_value());
    ASSERT_EQ(r_ca->first, 201) << r_ca->second.dump();
    std::string aid = r_ca->second.value("id", "");
    ASSERT_FALSE(aid.empty());
    // Verify no encrypted_token in response
    ASSERT_FALSE(r_ca->second.contains("encrypted_token"));

    // Create transaction
    json create_tx = {{"amount_cents",-5000},{"description","Coffee"}};
    auto r_ct = http_json(client, "POST",
        base_url() + "/accounts/" + aid + "/transactions", tok, &create_tx);
    ASSERT_TRUE(r_ct.has_value());
    ASSERT_EQ(r_ct->first, 201) << r_ct->second.dump();
    std::string txid = r_ct->second.value("id", "");
    ASSERT_FALSE(txid.empty());
    EXPECT_EQ(r_ct->second.value("amount_cents", int64_t{0}), int64_t{-5000});
    EXPECT_EQ(r_ct->second.value("description", ""), "Coffee");

    // GET transactions list
    auto r_lt = http_json(client, "GET",
        base_url() + "/accounts/" + aid + "/transactions", tok);
    ASSERT_TRUE(r_lt.has_value());
    ASSERT_EQ(r_lt->first, 200);
    ASSERT_TRUE(r_lt->second.contains("items"));
    ASSERT_GE(r_lt->second["items"].size(), 1u);

    // DELETE entity
    auto r_de = http_json(client, "DELETE", base_url() + "/entities/" + eid, tok);
    ASSERT_TRUE(r_de.has_value());
    ASSERT_EQ(r_de->first, 200);
}

// --------------------------------------------------------------------------
// Test 2: UnauthenticatedRequests_Return401
// --------------------------------------------------------------------------

TEST_F(DataEndpointsFixture, UnauthenticatedRequests_Return401) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    // No Bearer token — all data endpoints must return 401.
    auto r1 = http_json(client, "GET", base_url() + "/entities");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->first, 401);

    auto r2 = http_json(client, "GET", base_url() + "/entities/doesnotexist");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->first, 401);

    json body = {{"name","x"},{"kind","individual"}};
    auto r3 = http_json(client, "POST", base_url() + "/entities", "", &body);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->first, 401);

    // Invalid (garbage) Bearer token
    auto r4 = http_json(client, "GET", base_url() + "/entities", "notarealtoken000000000000000000000000000000000000000000000000000000");
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->first, 401);
}

// --------------------------------------------------------------------------
// Test 3: CrossUserAccess_Returns403
// --------------------------------------------------------------------------

TEST_F(DataEndpointsFixture, CrossUserAccess_Returns403) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok_a = enroll_and_login(client, "alice@example.com", "alicepass123");
    ASSERT_FALSE(tok_a.empty()) << "Alice enroll/login failed";

    std::string tok_b = enroll_and_login(client, "bob@example.com", "bobpass456");
    ASSERT_FALSE(tok_b.empty()) << "Bob enroll/login failed";

    // Alice creates an entity
    json create_ent = {{"name","Alice Entity"},{"kind","Individual"}};
    auto r = http_json(client, "POST", base_url() + "/entities", tok_a, &create_ent);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->first, 201) << r->second.dump();
    std::string alice_eid = r->second.value("id", "");
    ASSERT_FALSE(alice_eid.empty());

    // Bob tries to GET Alice's entity — must get 403
    auto r_get = http_json(client, "GET",
        base_url() + "/entities/" + alice_eid, tok_b);
    ASSERT_TRUE(r_get.has_value());
    EXPECT_EQ(r_get->first, 403) << r_get->second.dump();

    // Bob tries to DELETE Alice's entity — must get 403
    auto r_del = http_json(client, "DELETE",
        base_url() + "/entities/" + alice_eid, tok_b);
    ASSERT_TRUE(r_del.has_value());
    EXPECT_EQ(r_del->first, 403);

    // Bob tries to list Alice's accounts — must get 403
    auto r_accs = http_json(client, "GET",
        base_url() + "/entities/" + alice_eid + "/accounts", tok_b);
    ASSERT_TRUE(r_accs.has_value());
    EXPECT_EQ(r_accs->first, 403);
}

// --------------------------------------------------------------------------
// Test 4: CategoriesCrud
// --------------------------------------------------------------------------

TEST_F(DataEndpointsFixture, CategoriesCrud) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok = enroll_and_login(client, "catuser@example.com", "catpass789");
    ASSERT_FALSE(tok.empty());

    // Create entity
    json create_ent = {{"name","Cat Entity"},{"kind","Individual"}};
    auto r_ent = http_json(client, "POST", base_url() + "/entities", tok, &create_ent);
    ASSERT_TRUE(r_ent.has_value());
    ASSERT_EQ(r_ent->first, 201);
    std::string eid = r_ent->second.value("id", "");
    ASSERT_FALSE(eid.empty());

    // Create category
    json create_cat = {{"name","Food"},{"kind","expense"}};
    auto r_cc = http_json(client, "POST",
        base_url() + "/entities/" + eid + "/categories", tok, &create_cat);
    ASSERT_TRUE(r_cc.has_value());
    ASSERT_EQ(r_cc->first, 201) << r_cc->second.dump();
    std::string cat_id = r_cc->second.value("id", "");
    ASSERT_FALSE(cat_id.empty());
    EXPECT_EQ(r_cc->second.value("name", ""), "Food");
    EXPECT_EQ(r_cc->second.value("kind", ""), "expense");

    // List categories
    auto r_lc = http_json(client, "GET",
        base_url() + "/entities/" + eid + "/categories", tok);
    ASSERT_TRUE(r_lc.has_value());
    ASSERT_EQ(r_lc->first, 200);
    ASSERT_TRUE(r_lc->second.contains("items"));
    ASSERT_GE(r_lc->second["items"].size(), 1u);

    // GET single category
    auto r_gc = http_json(client, "GET",
        base_url() + "/categories/" + cat_id, tok);
    ASSERT_TRUE(r_gc.has_value());
    ASSERT_EQ(r_gc->first, 200);
    EXPECT_EQ(r_gc->second.value("id", ""), cat_id);

    // PUT category
    json upd_cat = {{"name","Dining"}};
    auto r_uc = http_json(client, "PUT",
        base_url() + "/categories/" + cat_id, tok, &upd_cat);
    ASSERT_TRUE(r_uc.has_value());
    ASSERT_EQ(r_uc->first, 200);
    EXPECT_EQ(r_uc->second.value("name", ""), "Dining");

    // DELETE category
    auto r_dc = http_json(client, "DELETE",
        base_url() + "/categories/" + cat_id, tok);
    ASSERT_TRUE(r_dc.has_value());
    ASSERT_EQ(r_dc->first, 200);
}

// --------------------------------------------------------------------------
// Test 5: BudgetsCrud
// --------------------------------------------------------------------------

TEST_F(DataEndpointsFixture, BudgetsCrud) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok = enroll_and_login(client, "buduser@example.com", "budpass321");
    ASSERT_FALSE(tok.empty());

    // Create entity
    json create_ent = {{"name","Budget Entity"},{"kind","Individual"}};
    auto r_ent = http_json(client, "POST", base_url() + "/entities", tok, &create_ent);
    ASSERT_TRUE(r_ent.has_value());
    ASSERT_EQ(r_ent->first, 201);
    std::string eid = r_ent->second.value("id", "");
    ASSERT_FALSE(eid.empty());

    // Create budget
    json create_bud = {
        {"amount_cents", 50000},
        {"period_start_unix", int64_t{1748736000}},  // 2025-06-01
        {"period_end_unix",   int64_t{1751327999}}   // 2025-06-30
    };
    auto r_cb = http_json(client, "POST",
        base_url() + "/entities/" + eid + "/budgets", tok, &create_bud);
    ASSERT_TRUE(r_cb.has_value());
    ASSERT_EQ(r_cb->first, 201) << r_cb->second.dump();
    std::string bud_id = r_cb->second.value("id", "");
    ASSERT_FALSE(bud_id.empty());
    EXPECT_EQ(r_cb->second.value("amount_cents", int64_t{0}), int64_t{50000});

    // List budgets
    auto r_lb = http_json(client, "GET",
        base_url() + "/entities/" + eid + "/budgets", tok);
    ASSERT_TRUE(r_lb.has_value());
    ASSERT_EQ(r_lb->first, 200);
    ASSERT_TRUE(r_lb->second.contains("items"));
    ASSERT_GE(r_lb->second["items"].size(), 1u);

    // GET single budget
    auto r_gb = http_json(client, "GET",
        base_url() + "/budgets/" + bud_id, tok);
    ASSERT_TRUE(r_gb.has_value());
    ASSERT_EQ(r_gb->first, 200);
    EXPECT_EQ(r_gb->second.value("id", ""), bud_id);

    // PUT budget
    json upd_bud = {{"amount_cents", 75000}};
    auto r_ub = http_json(client, "PUT",
        base_url() + "/budgets/" + bud_id, tok, &upd_bud);
    ASSERT_TRUE(r_ub.has_value());
    ASSERT_EQ(r_ub->first, 200);
    EXPECT_EQ(r_ub->second.value("amount_cents", int64_t{0}), int64_t{75000});

    // DELETE budget
    auto r_db = http_json(client, "DELETE",
        base_url() + "/budgets/" + bud_id, tok);
    ASSERT_TRUE(r_db.has_value());
    ASSERT_EQ(r_db->first, 200);
}

#endif // !_WIN32

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
