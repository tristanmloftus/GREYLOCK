// tests/test_supplier_map_endpoint.cpp — Phase 5 integration tests for
// GET /supplier-map.
//
// Mirrors the pattern in test_data_endpoints.cpp:
//   - POSIX-only (fork+exec); test is GTEST_SKIPed on Windows.
//   - Boots TerminalFinanceServer as a child process bound to port 19846
//     (one above the data-endpoint test port to allow parallel ctest runs).
//   - Drives auth via the canonical /auth/enroll + /auth/login flow.
//
// Test cases:
//   1. Unauthenticated GET /supplier-map -> 401
//   2. Invalid Bearer token GET /supplier-map -> 401
//   3. Authenticated GET /supplier-map -> 200 + canonical JSON
//   4. Body matches the on-disk fixture (rules array, version, schema)
//   5. Missing-file fallback -> 500 with {"error":"supplier_map_unavailable"}

#include <gtest/gtest.h>

#include "src/services/http/CurlHttpClient.h"
#include "src/services/IHttpClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifndef _WIN32
#  include <cerrno>
#  include <cstdio>
#  include <cstring>
#  include <csignal>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <sodium.h>
#  include "server/auth/HmacSha1.h"
#endif

using json = nlohmann::json;
using namespace std::chrono_literals;

#ifndef TF_FIXTURES_DIR
#  define TF_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef TF_SERVER_BIN
#  define TF_SERVER_BIN "./TerminalFinanceServer"
#endif
#ifndef TF_SUPPLIER_MAP_FIXTURE
#  define TF_SUPPLIER_MAP_FIXTURE "data/supplier_map.json"
#endif

static const std::string kTestCert       = TF_FIXTURES_DIR "/test-cert.pem";
static const std::string kTestKey        = TF_FIXTURES_DIR "/test-key.pem";
static const std::string kTestCA         = TF_FIXTURES_DIR "/test-ca.pem";
static const std::string kServerBin      = TF_SERVER_BIN;
static const std::string kSupplierFixture = TF_SUPPLIER_MAP_FIXTURE;

static constexpr int kSupplierMapPort = 19846;

// --------------------------------------------------------------------------
// CA-injecting HTTP client (same pattern as test_data_endpoints.cpp)
// --------------------------------------------------------------------------

class CaInjectingClient : public IHttpClient {
public:
    explicit CaInjectingClient(const std::string& ca_path) : ca_path_(ca_path) {}

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

// TOTP helpers — duplicated from test_data_endpoints.cpp to keep this test
// stand-alone.  Pulling them into a shared header would balloon the
// touched-files set; the helpers are < 40 lines.

static std::vector<uint8_t> base32_decode_sm(const std::string& s) {
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

static std::string extract_secret_b32_sm(const std::string& uri) {
    auto pos = uri.find("secret=");
    if (pos == std::string::npos) return "";
    pos += 7;
    auto end = uri.find('&', pos);
    if (end == std::string::npos) end = uri.size();
    return uri.substr(pos, end - pos);
}

static int compute_test_totp_sm(const std::vector<uint8_t>& secret, int64_t t_unix) {
    uint64_t counter = static_cast<uint64_t>(t_unix) / 30;
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; --i) {
        counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    auto hmac = tf::auth::hmacsha1::hmac(
        secret.data(), secret.size(), counter_bytes, 8);
    uint8_t offset = hmac[19] & 0x0F;
    uint32_t p = ((hmac[offset]     & 0x7F) << 24)
               | ((hmac[offset + 1] & 0xFF) << 16)
               | ((hmac[offset + 2] & 0xFF) <<  8)
               |  (hmac[offset + 3] & 0xFF);
    return static_cast<int>(p % 1000000);
}

static std::optional<std::pair<int, json>> http_json_sm(
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
// Fixture
// --------------------------------------------------------------------------

class SupplierMapEndpointFixture : public ::testing::Test {
protected:
    pid_t       server_pid_{-1};
    std::string db_path_;
    std::string supplier_map_env_;

    // Per-test override of the supplier-map path. Set in SetUp() to the
    // canonical fixture; individual tests may construct their own fixture
    // and override before forking the server.
    void boot_server(const std::string& supplier_path) {
        db_path_ = "/tmp/tf_supplier_map_test_"
            + std::to_string(getpid())
            + "_" + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch().count())
            + ".db";
        supplier_map_env_ = supplier_path;

        server_pid_ = ::fork();
        ASSERT_NE(server_pid_, -1) << "fork() failed: " << std::strerror(errno);

        if (server_pid_ == 0) {
            ::setenv("TF_SERVER_PORT",        std::to_string(kSupplierMapPort).c_str(), 1);
            ::setenv("TF_SERVER_CERT_PATH",   kTestCert.c_str(), 1);
            ::setenv("TF_SERVER_KEY_PATH",    kTestKey.c_str(),  1);
            ::setenv("TF_SERVER_BIND_ADDR",   "127.0.0.1",       1);
            ::setenv("TF_DB_PATH",            db_path_.c_str(),  1);
            ::setenv("TF_SUPPLIER_MAP_PATH",  supplier_map_env_.c_str(), 1);

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
        if (!db_path_.empty()) {
            ::unlink(db_path_.c_str());
        }
    }

    std::string base_url() const {
        return "https://127.0.0.1:" + std::to_string(kSupplierMapPort);
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
        while (!tok.empty() &&
               (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) {
            tok.pop_back();
        }
        return tok;
    }

    std::string enroll_and_login(CaInjectingClient& client,
                                  const std::string& email,
                                  const std::string& passphrase)
    {
        std::string enroll_token = mint_token(email);
        if (enroll_token.size() != 64u) return "";

        json enroll_body = {{"token", enroll_token},
                             {"email", email},
                             {"passphrase", passphrase}};
        auto enroll_resp = http_json_sm(client, "POST",
            base_url() + "/auth/enroll", "", &enroll_body);
        if (!enroll_resp || enroll_resp->first != 200) return "";

        std::string prov_uri = enroll_resp->second.value("totp_provisioning_uri", "");
        std::string b32 = extract_secret_b32_sm(prov_uri);
        if (b32.empty()) return "";
        auto secret_bytes = base32_decode_sm(b32);

        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        int totp = compute_test_totp_sm(secret_bytes, now);

        json login_body = {{"email", email},
                            {"passphrase", passphrase},
                            {"totp_code", totp}};
        auto login_resp = http_json_sm(client, "POST",
            base_url() + "/auth/login", "", &login_body);
        if (!login_resp || login_resp->first != 200) return "";

        return login_resp->second.value("session_token", "");
    }
};

// --------------------------------------------------------------------------
// Test 1: Unauthenticated requests return 401.
// --------------------------------------------------------------------------

TEST_F(SupplierMapEndpointFixture, NoSession_Returns401) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    boot_server(kSupplierFixture);
    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    auto r = http_json_sm(client, "GET", base_url() + "/supplier-map");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, 401);
}

TEST_F(SupplierMapEndpointFixture, InvalidSession_Returns401) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    boot_server(kSupplierFixture);
    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    // 64-char hex but not a session that the server ever issued.
    const std::string garbage(64, 'a');
    auto r = http_json_sm(client, "GET", base_url() + "/supplier-map", garbage);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, 401);
}

// --------------------------------------------------------------------------
// Test 2: Authenticated GET returns 200 + canonical JSON matching the
//         on-disk fixture.
// --------------------------------------------------------------------------

TEST_F(SupplierMapEndpointFixture, ValidSession_Returns200AndCanonicalJson) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    boot_server(kSupplierFixture);
    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok = enroll_and_login(client,
        "supplier-map@example.com", "supplierpass-abc-123");
    ASSERT_FALSE(tok.empty()) << "enroll_and_login failed";

    auto r = http_json_sm(client, "GET", base_url() + "/supplier-map", tok);
    ASSERT_TRUE(r.has_value()) << "request failed at transport layer";
    ASSERT_EQ(r->first, 200) << r->second.dump();

    const json& body = r->second;
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.value("version", 0), 1);
    ASSERT_TRUE(body.contains("rules"));
    ASSERT_TRUE(body["rules"].is_array());
    EXPECT_FALSE(body["rules"].empty());

    // Spot-check the schema of every rule and confirm a known-good ticker
    // (STARBUCKS -> SBUX) shows up.
    bool saw_starbucks = false;
    bool saw_usps      = false;
    for (const auto& rule : body["rules"]) {
        ASSERT_TRUE(rule.is_object());
        EXPECT_TRUE(rule.contains("match"));
        EXPECT_TRUE(rule.contains("match_kind"));
        EXPECT_TRUE(rule.contains("supplier"));
        EXPECT_TRUE(rule.contains("ticker"));
        if (rule.value("supplier", "") == "Starbucks Corp") {
            EXPECT_EQ(rule.value("ticker", ""), "SBUX");
            saw_starbucks = true;
        }
        if (rule.value("supplier", "") == "USPS") {
            // USPS has no public listing -- ticker must be empty.
            EXPECT_EQ(rule.value("ticker", "_unset"), "");
            saw_usps = true;
        }
    }
    EXPECT_TRUE(saw_starbucks)
        << "Expected a Starbucks/SBUX rule in the response";
    EXPECT_TRUE(saw_usps)
        << "Expected a USPS rule with empty ticker in the response";
}

// --------------------------------------------------------------------------
// Test 3: Missing file -> 500 supplier_map_unavailable.
// --------------------------------------------------------------------------

TEST_F(SupplierMapEndpointFixture, MissingFile_Returns500NonLeaky) {
    if (sodium_init() < 0) GTEST_SKIP() << "sodium_init() failed";

    boot_server("/tmp/tf__definitely_not_present__.json");
    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server()) << "Server did not start on " << base_url();

    std::string tok = enroll_and_login(client,
        "missing@example.com", "missingpass-abc-123");
    ASSERT_FALSE(tok.empty());

    auto r = http_json_sm(client, "GET", base_url() + "/supplier-map", tok);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, 500);
    ASSERT_TRUE(r->second.is_object());
    EXPECT_EQ(r->second.value("error", ""), "supplier_map_unavailable");
    // F-3 / no-leakage: body must not include the disk path or system msg.
    EXPECT_FALSE(r->second.contains("message"));
    EXPECT_FALSE(r->second.contains("path"));
}

#else // _WIN32

TEST(SupplierMapEndpointFixture, SkippedOnWindows) {
    GTEST_SKIP() << "test_supplier_map_endpoint requires fork+exec (POSIX)";
}

#endif // _WIN32

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
