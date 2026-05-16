// tests/test_auth_endpoints.cpp — Phase 3 auth endpoint integration tests.
//
// Launches greylock-server as a subprocess (port 19844), drives the
// full auth flow via CurlHttpClient:
//
//   1. Mint enrollment token (via --mint-enrollment-token flag on the binary)
//   2. POST /auth/enroll
//   3. POST /auth/login (correct creds, TOTP code computed from provisioning URI)
//   4. GET  /auth/whoami (valid session → 200)
//   5. POST /auth/logout
//   6. GET  /auth/whoami (revoked session → 401)
//
// POSIX-only (fork+exec). Skipped on Windows.
//
// Test cases:
//   1. FullAuthFlow — end-to-end happy path (POSIX only)

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
#endif

using json = nlohmann::json;
using namespace std::chrono_literals;

// --------------------------------------------------------------------------
// Paths injected by CMake
// --------------------------------------------------------------------------

#ifndef TF_FIXTURES_DIR
#  define TF_FIXTURES_DIR "tests/fixtures"
#endif

#ifndef TF_SERVER_BIN
#  define TF_SERVER_BIN "./greylock-server"
#endif

static const std::string kTestCert  = TF_FIXTURES_DIR "/test-cert.pem";
static const std::string kTestKey   = TF_FIXTURES_DIR "/test-key.pem";
static const std::string kTestCA    = TF_FIXTURES_DIR "/test-ca.pem";
static const std::string kServerBin = TF_SERVER_BIN;

static constexpr int kAuthLivePort = 19844;

// --------------------------------------------------------------------------
// CA-injecting HTTP client (same pattern as BackendClientTests).
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

// --------------------------------------------------------------------------
// Simple TOTP compute (re-implementation using libsodium primitives) for
// the test so it doesn't need to link the server/auth/Totp.cpp directly.
// We parse the secret from the provisioning URI and compute the current code.
// --------------------------------------------------------------------------

#ifndef _WIN32
#  include <sodium.h>
#  include "../server/auth/HmacSha1.h"

// Decode uppercase base32 (no padding) → bytes.
static std::vector<uint8_t> base32_decode(const std::string& s) {
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

// Extract ?secret=<base32> from provisioning URI.
static std::string extract_secret_b32(const std::string& uri) {
    auto pos = uri.find("secret=");
    if (pos == std::string::npos) return "";
    pos += 7;
    auto end = uri.find('&', pos);
    if (end == std::string::npos) end = uri.size();
    return uri.substr(pos, end - pos);
}

// Compute 6-digit TOTP code using HMAC-SHA1 (mirrors Totp.cpp).
static int compute_test_totp(const std::vector<uint8_t>& secret, int64_t t_unix) {
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
#endif // !_WIN32

// --------------------------------------------------------------------------
// Live server fixture
// --------------------------------------------------------------------------

#ifndef _WIN32

class AuthEndpointsFixture : public ::testing::Test {
protected:
    pid_t server_pid_{-1};
    std::string db_path_;

    void SetUp() override {
        // Use a temp DB path specific to this test run to avoid conflicts.
        db_path_ = "/tmp/tf_auth_test_" + std::to_string(getpid()) + ".db";

        server_pid_ = ::fork();
        ASSERT_NE(server_pid_, -1) << "fork() failed: " << std::strerror(errno);

        if (server_pid_ == 0) {
            // ---- child: become the server ----
            ::setenv("TF_SERVER_PORT",      std::to_string(kAuthLivePort).c_str(), 1);
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
        // parent: wait for server to become ready.
    }

    void TearDown() override {
        if (server_pid_ > 0) {
            ::kill(server_pid_, SIGTERM);
            int status = 0;
            ::waitpid(server_pid_, &status, 0);
            server_pid_ = -1;
        }
        // Clean up temp DB.
        ::unlink(db_path_.c_str());
    }

    std::string base_url() const {
        return "https://127.0.0.1:" + std::to_string(kAuthLivePort);
    }

    // Poll until server responds to /healthz (up to 5 s).
    bool wait_for_server(const CaInjectingClient& client_ref) {
        CaInjectingClient poller(kTestCA);
        for (int i = 0; i < 100; ++i) {
            HttpRequest req;
            req.method = "GET";
            req.url    = base_url() + "/healthz";
            auto resp  = poller.send(req);
            if (resp && resp->status_code == 200) return true;
            std::this_thread::sleep_for(50ms);
        }
        (void)client_ref;
        return false;
    }

    // Mint an enrollment token by running the server binary with the admin flag.
    std::string mint_token(const std::string& email) {
        // Build the command: pipe stdout to capture the token.
        std::string cmd = kServerBin
            + " --mint-enrollment-token " + email
            + " 2>/dev/null";
        // Set env for the subprocess.
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
        // Trim trailing newline/whitespace.
        while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r'
                                || tok.back() == ' ')) {
            tok.pop_back();
        }
        return tok;
    }
};

// Helper: send a POST with JSON body, return parsed response.
static std::optional<std::pair<int, json>> post_json(
    CaInjectingClient& client,
    const std::string& url,
    const json& body)
{
    HttpRequest req;
    req.method = "POST";
    req.url    = url;
    req.headers["Content-Type"] = "application/json";
    req.body = body.dump();
    auto resp = client.send(req);
    if (!resp) return std::nullopt;
    try {
        return std::make_pair(static_cast<int>(resp->status_code),
                              json::parse(resp->body));
    } catch (...) {
        return std::make_pair(static_cast<int>(resp->status_code), json{});
    }
}

static std::optional<std::pair<int, json>> get_json(
    CaInjectingClient& client,
    const std::string& url,
    const std::string& bearer_token = "")
{
    HttpRequest req;
    req.method = "GET";
    req.url    = url;
    if (!bearer_token.empty()) {
        req.headers["Authorization"] = "Bearer " + bearer_token;
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

static std::optional<std::pair<int, json>> post_json_auth(
    CaInjectingClient& client,
    const std::string& url,
    const json& body,
    const std::string& bearer_token)
{
    HttpRequest req;
    req.method = "POST";
    req.url    = url;
    req.headers["Content-Type"]  = "application/json";
    req.headers["Authorization"] = "Bearer " + bearer_token;
    req.body = body.dump();
    auto resp = client.send(req);
    if (!resp) return std::nullopt;
    try {
        return std::make_pair(static_cast<int>(resp->status_code),
                              json::parse(resp->body));
    } catch (...) {
        return std::make_pair(static_cast<int>(resp->status_code), json{});
    }
}

// 2. Login_NonexistentEmailIsTimingEqualized
//
// Verifies C-1: login with a nonexistent email takes >300 ms (Argon2id ran),
// matching the latency of login with a real-but-wrong-passphrase.
// Uses a coarse threshold — goal is "crypto ran", not "exactly equal".
TEST_F(AuthEndpointsFixture, Login_NonexistentEmailIsTimingEqualized) {
    if (sodium_init() < 0) {
        GTEST_SKIP() << "sodium_init() failed";
    }

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server(client))
        << "Server did not start on " << base_url();

    // --- Arm 1: nonexistent email ---
    auto t0 = std::chrono::steady_clock::now();
    auto resp_ne = post_json(client, base_url() + "/auth/login", {
        {"email",      "nobody-does-not-exist@example.com"},
        {"passphrase", "some-passphrase"},
        {"totp_code",  123456}
    });
    auto t1 = std::chrono::steady_clock::now();
    auto ms_ne = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    ASSERT_TRUE(resp_ne.has_value());
    EXPECT_EQ(resp_ne->first, 401);
    EXPECT_EQ(resp_ne->second.value("error", ""), "auth_failed");

    // Must have taken at least 300 ms (Argon2id work was performed).
    EXPECT_GE(ms_ne, 300)
        << "Login with nonexistent email returned in " << ms_ne
        << " ms — expected >=300 ms (Argon2id must run on user-not-found path)";

    // --- Arm 2: real email, wrong passphrase ---
    // Enroll a user first so there is a real email in the DB.
    const std::string real_email = "timing-test-user@example.com";
    std::string enroll_token = mint_token(real_email);
    ASSERT_EQ(enroll_token.size(), 64u) << "mint_token returned: '" << enroll_token << "'";

    auto enroll_resp = post_json(client, base_url() + "/auth/enroll", {
        {"token",      enroll_token},
        {"email",      real_email},
        {"passphrase", "correct-passphrase-for-timing-test"}
    });
    ASSERT_TRUE(enroll_resp.has_value());
    ASSERT_EQ(enroll_resp->first, 200) << enroll_resp->second.dump();

    auto t2 = std::chrono::steady_clock::now();
    auto resp_wp = post_json(client, base_url() + "/auth/login", {
        {"email",      real_email},
        {"passphrase", "wrong-passphrase"},
        {"totp_code",  999999}
    });
    auto t3 = std::chrono::steady_clock::now();
    auto ms_wp = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

    ASSERT_TRUE(resp_wp.has_value());
    EXPECT_EQ(resp_wp->first, 401);
    EXPECT_EQ(resp_wp->second.value("error", ""), "auth_failed");

    EXPECT_GE(ms_wp, 300)
        << "Login with wrong passphrase returned in " << ms_wp
        << " ms — expected >=300 ms (Argon2id must run)";
}

// 3. Enroll_EmailMismatch_ReturnsSameErrorAsInvalidToken
//
// Verifies H-1 + H-2: enrolling with a valid token but the wrong email returns
// the same 400 {"error":"invalid_enrollment_token"} as a garbage token, and
// does NOT consume the token (original enrollment with the correct email still
// works afterward).
TEST_F(AuthEndpointsFixture, Enroll_EmailMismatch_ReturnsSameErrorAsInvalidToken) {
    if (sodium_init() < 0) {
        GTEST_SKIP() << "sodium_init() failed";
    }

    CaInjectingClient client(kTestCA);
    ASSERT_TRUE(wait_for_server(client))
        << "Server did not start on " << base_url();

    const std::string correct_email = "a@example.com";
    const std::string wrong_email   = "b@example.com";

    // Mint a valid token for correct_email.
    std::string valid_token = mint_token(correct_email);
    ASSERT_EQ(valid_token.size(), 64u) << "mint_token returned: '" << valid_token << "'";

    // --- Sub-case A: try enrolling with wrong email + valid token ---
    // Should look identical to an invalid token response.
    auto resp_mismatch = post_json(client, base_url() + "/auth/enroll", {
        {"token",      valid_token},
        {"email",      wrong_email},
        {"passphrase", "some-passphrase"}
    });
    ASSERT_TRUE(resp_mismatch.has_value());
    EXPECT_EQ(resp_mismatch->first, 400)
        << "Email mismatch should return 400, got: " << resp_mismatch->second.dump();
    EXPECT_EQ(resp_mismatch->second.value("error", ""), "invalid_enrollment_token")
        << "Email mismatch error key should be 'invalid_enrollment_token', got: "
        << resp_mismatch->second.dump();

    // --- Sub-case B: same garbage-token response for reference ---
    std::string garbage_token(64, '0'); // all zeros — cannot exist
    auto resp_garbage = post_json(client, base_url() + "/auth/enroll", {
        {"token",      garbage_token},
        {"email",      correct_email},
        {"passphrase", "some-passphrase"}
    });
    ASSERT_TRUE(resp_garbage.has_value());
    EXPECT_EQ(resp_garbage->first, 400);
    EXPECT_EQ(resp_garbage->second.value("error", ""), "invalid_enrollment_token");

    // Both mismatch and garbage must return the same HTTP status + error key.
    EXPECT_EQ(resp_mismatch->first, resp_garbage->first)
        << "HTTP status must be identical";
    EXPECT_EQ(resp_mismatch->second.value("error", ""),
              resp_garbage->second.value("error", ""))
        << "Error value must be identical";

    // --- Sub-case C: verify the original token was NOT consumed ---
    // Enrolling with the correct email + same token must still succeed.
    auto resp_correct = post_json(client, base_url() + "/auth/enroll", {
        {"token",      valid_token},
        {"email",      correct_email},
        {"passphrase", "correct-passphrase-after-mismatch-attempt"}
    });
    ASSERT_TRUE(resp_correct.has_value());
    EXPECT_EQ(resp_correct->first, 200)
        << "Enroll with correct email after a mismatch attempt should succeed; got: "
        << resp_correct->second.dump();
    EXPECT_TRUE(resp_correct->second.contains("user_id"))
        << "Response missing user_id: " << resp_correct->second.dump();
    EXPECT_TRUE(resp_correct->second.contains("totp_provisioning_uri"))
        << "Response missing totp_provisioning_uri: " << resp_correct->second.dump();
}

// 1. FullAuthFlow
TEST_F(AuthEndpointsFixture, FullAuthFlow) {
    if (sodium_init() < 0) {
        GTEST_SKIP() << "sodium_init() failed";
    }

    CaInjectingClient client(kTestCA);

    // Wait for server.
    ASSERT_TRUE(wait_for_server(client))
        << "Server did not start on " << base_url();

    const std::string email      = "rory@example.com";
    const std::string passphrase = "hunter2hunter2hunter2";

    // Step 1: Mint enrollment token.
    std::string enroll_token = mint_token(email);
    ASSERT_EQ(enroll_token.size(), 64u)
        << "Expected 64-char hex enrollment token, got: '" << enroll_token << "'";

    // Step 2: POST /auth/enroll
    auto enroll_resp = post_json(client, base_url() + "/auth/enroll", {
        {"token",      enroll_token},
        {"email",      email},
        {"passphrase", passphrase}
    });
    ASSERT_TRUE(enroll_resp.has_value());
    ASSERT_EQ(enroll_resp->first, 200)
        << "Enroll failed: " << enroll_resp->second.dump();
    ASSERT_TRUE(enroll_resp->second.contains("user_id"));
    ASSERT_TRUE(enroll_resp->second.contains("totp_provisioning_uri"));

    std::string user_id = enroll_resp->second["user_id"].get<std::string>();
    std::string prov_uri = enroll_resp->second["totp_provisioning_uri"].get<std::string>();

    // Parse TOTP secret from URI and compute current code.
    std::string b32 = extract_secret_b32(prov_uri);
    ASSERT_FALSE(b32.empty()) << "Could not extract secret from: " << prov_uri;
    auto secret_bytes = base32_decode(b32);
    ASSERT_FALSE(secret_bytes.empty());

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    int totp_code = compute_test_totp(secret_bytes, now);

    // Step 3: POST /auth/login
    auto login_resp = post_json(client, base_url() + "/auth/login", {
        {"email",      email},
        {"passphrase", passphrase},
        {"totp_code",  totp_code}
    });
    ASSERT_TRUE(login_resp.has_value());
    ASSERT_EQ(login_resp->first, 200)
        << "Login failed: " << login_resp->second.dump();
    ASSERT_TRUE(login_resp->second.contains("session_token"));
    ASSERT_TRUE(login_resp->second.contains("user_id"));

    std::string session_token = login_resp->second["session_token"].get<std::string>();
    EXPECT_EQ(login_resp->second["user_id"].get<std::string>(), user_id);

    // Step 4: GET /auth/whoami (valid session → 200)
    auto whoami1 = get_json(client, base_url() + "/auth/whoami", session_token);
    ASSERT_TRUE(whoami1.has_value());
    ASSERT_EQ(whoami1->first, 200)
        << "Whoami failed: " << whoami1->second.dump();
    EXPECT_EQ(whoami1->second["user_id"].get<std::string>(), user_id);
    EXPECT_EQ(whoami1->second["email"].get<std::string>(), email);

    // Step 5: POST /auth/logout
    auto logout_resp = post_json_auth(client, base_url() + "/auth/logout",
                                      json::object(), session_token);
    ASSERT_TRUE(logout_resp.has_value());
    EXPECT_EQ(logout_resp->first, 200);

    // Step 6: GET /auth/whoami (revoked session → 401)
    auto whoami2 = get_json(client, base_url() + "/auth/whoami", session_token);
    ASSERT_TRUE(whoami2.has_value());
    EXPECT_EQ(whoami2->first, 401);
}

#endif // !_WIN32

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
