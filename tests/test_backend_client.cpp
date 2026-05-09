// tests/test_backend_client.cpp
//
// Unit + integration tests for BackendClient (Phase 2.C).
//
// Unit tests use FakeHttpClient — no network required.
// Integration test (Healthz_LiveRoundTrip) forks+execs TerminalFinanceServer
// on a fixed ephemeral-ish port (19843), polls /healthz via BackendClient
// until the server is up, asserts healthz() returns true, then SIGTERMs the
// server.
//
// Test cases (13 total):
//   1.  Get_2xxJsonReturnsJson
//   2.  Get_2xxInvalidJsonReturnsBadResponse
//   3.  Get_NulloptReturnsTransportError
//   4.  Get_401ReturnsUnauthorized
//   5.  Get_404ReturnsNotFound
//   6.  Get_409ReturnsConflict
//   7.  Get_429ReturnsRateLimited
//   8.  Get_500ReturnsServerError
//   9.  Get_AttachesAuthHeaderWhenTokenPresent
//  10.  Get_NoAuthHeaderWhenTokenAbsent
//  11.  Construction_RejectsHttpUrl
//  12.  Construction_AcceptsHttpsUrl
//  13.  Healthz_LiveRoundTrip  (integration, POSIX only)

#include <gtest/gtest.h>

#include "../src/services/BackendClient.h"
#include "../src/services/IHttpClient.h"
#include "../src/services/http/CurlHttpClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#ifndef _WIN32
#  include <cerrno>
#  include <cstring>
#  include <csignal>
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
#  define TF_SERVER_BIN "./TerminalFinanceServer"
#endif

static const std::string kTestCert  = TF_FIXTURES_DIR "/test-cert.pem";
static const std::string kTestKey   = TF_FIXTURES_DIR "/test-key.pem";
static const std::string kTestCA    = TF_FIXTURES_DIR "/test-ca.pem";
static const std::string kServerBin = TF_SERVER_BIN;

// --------------------------------------------------------------------------
// FakeHttpClient
// Records the last outgoing request; returns a programmer-supplied response.
// Returning std::nullopt simulates a transport failure.
// --------------------------------------------------------------------------

class FakeHttpClient : public IHttpClient {
public:
    // Set before calling send(). nullopt → transport error.
    std::optional<HttpResponse> next_response;

    // Last request sent via send().
    HttpRequest last_request;

    std::optional<HttpResponse> send(const HttpRequest& req) override {
        last_request = req;
        return next_response;
    }
};

// --------------------------------------------------------------------------
// CA-aware fake: wraps CurlHttpClient and injects ca_bundle_path on every
// request.  Used only in the live round-trip test so BackendClient can reach
// the test server (which presents a mkcert-issued cert) without modifying
// BackendClient's interface.
// --------------------------------------------------------------------------

class CaInjectingClient : public IHttpClient {
public:
    explicit CaInjectingClient(const std::string& ca_path)
        : ca_path_(ca_path), inner_() {}

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
// Helpers
// --------------------------------------------------------------------------

static std::shared_ptr<FakeHttpClient> make_fake() {
    return std::make_shared<FakeHttpClient>();
}

static HttpResponse make_resp(long status, const std::string& body) {
    HttpResponse r;
    r.status_code = status;
    r.body        = body;
    return r;
}

// --------------------------------------------------------------------------
// Unit tests
// --------------------------------------------------------------------------

// 1. 2xx with valid JSON body → returns parsed json.
TEST(BackendClientTest, Get_2xxJsonReturnsJson) {
    auto fake = make_fake();
    fake->next_response = make_resp(200, R"({"ok":true})");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/healthz");

    ASSERT_TRUE(std::holds_alternative<json>(result))
        << "Expected json variant, got BackendError";
    auto& j = std::get<json>(result);
    EXPECT_TRUE(j.contains("ok"));
    EXPECT_TRUE(j["ok"].get<bool>());
}

// 2. 2xx with invalid JSON body → BackendError{Kind::BadResponse}.
TEST(BackendClientTest, Get_2xxInvalidJsonReturnsBadResponse) {
    auto fake = make_fake();
    fake->next_response = make_resp(200, "not json");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/healthz");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::BadResponse);
}

// 3. nullopt from IHttpClient → BackendError{Kind::Transport}.
TEST(BackendClientTest, Get_NulloptReturnsTransportError) {
    auto fake = make_fake();
    fake->next_response = std::nullopt;

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/healthz");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    auto& err = std::get<BackendError>(result);
    EXPECT_EQ(err.kind, BackendError::Kind::Transport);
    EXPECT_EQ(err.http_status, 0L);
    EXPECT_EQ(err.code, "transport_failure");
}

// 4. 401 → Kind::Unauthorized.
TEST(BackendClientTest, Get_401ReturnsUnauthorized) {
    auto fake = make_fake();
    fake->next_response = make_resp(401, "");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/x");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::Unauthorized);
}

// 5. 404 → Kind::NotFound.
TEST(BackendClientTest, Get_404ReturnsNotFound) {
    auto fake = make_fake();
    fake->next_response = make_resp(404, "");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/x");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::NotFound);
}

// 6. 409 → Kind::Conflict.
TEST(BackendClientTest, Get_409ReturnsConflict) {
    auto fake = make_fake();
    fake->next_response = make_resp(409, "");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/x");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::Conflict);
}

// 7. 429 → Kind::RateLimited.
TEST(BackendClientTest, Get_429ReturnsRateLimited) {
    auto fake = make_fake();
    fake->next_response = make_resp(429, "");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/x");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    EXPECT_EQ(std::get<BackendError>(result).kind, BackendError::Kind::RateLimited);
}

// 8. 500 → Kind::ServerError; code and message parsed from JSON body.
TEST(BackendClientTest, Get_500ReturnsServerError) {
    auto fake = make_fake();
    fake->next_response = make_resp(500, R"({"code":"internal","message":"oops"})");

    BackendClient client(fake, "https://localhost:8443");
    auto result = client.get("/x");

    ASSERT_TRUE(std::holds_alternative<BackendError>(result));
    auto& err = std::get<BackendError>(result);
    EXPECT_EQ(err.kind, BackendError::Kind::ServerError);
    EXPECT_EQ(err.http_status, 500L);
    EXPECT_EQ(err.code, "internal");
    EXPECT_EQ(err.message, "oops");
}

// 9. Session token present → Authorization: Bearer <token> header sent.
TEST(BackendClientTest, Get_AttachesAuthHeaderWhenTokenPresent) {
    auto fake = make_fake();
    fake->next_response = make_resp(200, R"({"ok":true})");

    BackendClient client(fake, "https://localhost:8443");
    client.get("/x", std::string("tok123"));

    auto it = fake->last_request.headers.find("Authorization");
    ASSERT_NE(it, fake->last_request.headers.end())
        << "Authorization header not present in outgoing request";
    EXPECT_EQ(it->second, "Bearer tok123");
}

// 10. Session token absent → NO Authorization header.
TEST(BackendClientTest, Get_NoAuthHeaderWhenTokenAbsent) {
    auto fake = make_fake();
    fake->next_response = make_resp(200, R"({"ok":true})");

    BackendClient client(fake, "https://localhost:8443");
    client.get("/x");  // no session_token arg → defaults to nullopt

    auto it = fake->last_request.headers.find("Authorization");
    EXPECT_EQ(it, fake->last_request.headers.end())
        << "Authorization header must NOT be present when no session token supplied";
}

// 11. http:// URL rejected at construction (F-2 compliance).
TEST(BackendClientTest, Construction_RejectsHttpUrl) {
    auto fake = make_fake();
    EXPECT_THROW(
        BackendClient(fake, "http://localhost:8080"),
        std::invalid_argument
    ) << "BackendClient must reject non-HTTPS base URLs";
}

// 12. https:// URL accepted at construction.
TEST(BackendClientTest, Construction_AcceptsHttpsUrl) {
    auto fake = make_fake();
    EXPECT_NO_THROW(BackendClient(fake, "https://localhost:8443"));
}

// --------------------------------------------------------------------------
// Integration test: Healthz_LiveRoundTrip
// --------------------------------------------------------------------------
// Launches TerminalFinanceServer as a subprocess on port 19843, polls
// healthz() until the server responds (up to 5 seconds), asserts true, then
// SIGTERMs the server.  POSIX-only.
// --------------------------------------------------------------------------

#ifndef _WIN32

static constexpr int kLivePort = 19843;

class LiveServerFixture : public ::testing::Test {
protected:
    pid_t server_pid_{-1};

    void SetUp() override {
        server_pid_ = ::fork();
        ASSERT_NE(server_pid_, -1) << "fork() failed: " << std::strerror(errno);

        if (server_pid_ == 0) {
            // ---- child: become the server ----
            ::setenv("TF_SERVER_PORT",      std::to_string(kLivePort).c_str(), 1);
            ::setenv("TF_SERVER_CERT_PATH", kTestCert.c_str(), 1);
            ::setenv("TF_SERVER_KEY_PATH",  kTestKey.c_str(),  1);
            ::setenv("TF_SERVER_BIND_ADDR", "127.0.0.1",       1);

            // Redirect stdout/stderr to /dev/null so server logs don't
            // pollute GoogleTest output.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }

            ::execl(kServerBin.c_str(), kServerBin.c_str(), nullptr);
            // execl only returns on failure.
            ::_exit(127);
        }
        // ---- parent: server_pid_ is the child's PID ----
    }

    void TearDown() override {
        if (server_pid_ > 0) {
            ::kill(server_pid_, SIGTERM);
            int status = 0;
            ::waitpid(server_pid_, &status, 0);
            server_pid_ = -1;
        }
    }

    std::string base_url() const {
        return "https://127.0.0.1:" + std::to_string(kLivePort);
    }
};

TEST_F(LiveServerFixture, Healthz_LiveRoundTrip) {
    // Build a BackendClient backed by a CA-injecting curl client so TLS
    // verification uses the test CA that signed the server's cert.
    auto ca_http = std::make_shared<CaInjectingClient>(kTestCA);
    BackendClient client(ca_http, base_url());

    // Poll until the server responds or we time out (5 s, 50 ms intervals).
    constexpr int kMaxAttempts = 100;
    constexpr auto kPollInterval = 50ms;

    bool server_ready = false;
    for (int i = 0; i < kMaxAttempts; ++i) {
        auto result = client.healthz();
        if (std::holds_alternative<bool>(result) && std::get<bool>(result)) {
            server_ready = true;
            break;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    ASSERT_TRUE(server_ready)
        << "TerminalFinanceServer did not become healthy on "
        << base_url() << "/healthz within "
        << (kMaxAttempts * kPollInterval.count()) << " ms. "
        << "Check that the server binary exists at: " << kServerBin;

    // One final definitive call to confirm healthz() returns true.
    auto final_result = client.healthz();
    ASSERT_TRUE(std::holds_alternative<bool>(final_result));
    EXPECT_TRUE(std::get<bool>(final_result));
}

#endif  // !_WIN32

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
