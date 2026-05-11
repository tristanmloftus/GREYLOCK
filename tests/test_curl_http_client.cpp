#include <gtest/gtest.h>
#include "../src/services/IHttpClient.h"
#include "../src/services/http/CurlHttpClient.h"

#include <cstdlib>  // std::getenv

// --------------------------------------------------------------------------
// Interface contract tests (no network required)
// --------------------------------------------------------------------------

// CurlHttpClient must be constructable without throwing.
TEST(CurlHttpClientTest, Constructable) {
    EXPECT_NO_THROW({
        CurlHttpClient client;
        (void)client;
    });
}

// IHttpClient is purely abstract — verify CurlHttpClient satisfies the interface.
TEST(CurlHttpClientTest, ImplementsIHttpClient) {
    CurlHttpClient client;
    IHttpClient& base = client;
    (void)base;
    SUCCEED();
}

// HttpRequest defaults: 30-second timeout, empty headers, no body, empty method.
TEST(CurlHttpClientTest, HttpRequestDefaults) {
    HttpRequest req;
    EXPECT_EQ(req.timeout.count(), 30'000);
    EXPECT_TRUE(req.headers.empty());
    EXPECT_FALSE(req.body.has_value());
    EXPECT_TRUE(req.method.empty());
}

// HttpResponse defaults.
TEST(CurlHttpClientTest, HttpResponseDefaults) {
    HttpResponse resp;
    EXPECT_EQ(resp.status_code, 0L);
    EXPECT_TRUE(resp.headers.empty());
    EXPECT_TRUE(resp.body.empty());
}

// send() returns nullopt for an unreachable URL (connect error, not an HTTP error).
// Uses a non-routable address so the connect times out quickly.
TEST(CurlHttpClientTest, ReturnsNulloptOnConnectFailure) {
    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://192.0.2.1/";  // TEST-NET-1: guaranteed unreachable
    req.timeout = std::chrono::milliseconds{3'000};  // 3-second budget for this test

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value());
}

// send() returns nullopt for an invalid URL scheme.
TEST(CurlHttpClientTest, ReturnsNulloptOnBadUrl) {
    CurlHttpClient client;
    HttpRequest req;
    req.method = "GET";
    req.url    = "not-a-url://garbage";
    req.timeout = std::chrono::milliseconds{5'000};

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value());
}

// --------------------------------------------------------------------------
// Network integration tests — gated behind TF_NETWORK_TESTS=1
//
// These hit real HTTPS endpoints. They require an active internet connection.
// In offline CI runners, set TF_NETWORK_TESTS to any value other than "1"
// (or leave it unset) to skip these tests automatically.
//
// Usage:
//   TF_NETWORK_TESTS=1 ./CurlHttpClientTests
// --------------------------------------------------------------------------

static bool network_tests_enabled() {
    const char* val = std::getenv("TF_NETWORK_TESTS");
    return val != nullptr && std::string(val) == "1";
}

// GET https://www.google.com/generate_204 returns 204 with an empty body.
// A 204 confirms we reached the endpoint; it is the canonical canary URL.
TEST(CurlHttpClientTest, NetworkGet204) {
    if (!network_tests_enabled()) {
        GTEST_SKIP() << "Skipping network test (TF_NETWORK_TESTS != 1). "
                        "Set TF_NETWORK_TESTS=1 to enable.";
    }

    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://www.google.com/generate_204";
    req.timeout = std::chrono::milliseconds{15'000};

    auto result = client.send(req);

    ASSERT_TRUE(result.has_value()) << "Transport failure: send() returned nullopt";
    EXPECT_EQ(result->status_code, 204L)
        << "Expected HTTP 204, got " << result->status_code;
}

// GET https://example.com/ returns 200 with an HTML body.
TEST(CurlHttpClientTest, NetworkGetExampleCom) {
    if (!network_tests_enabled()) {
        GTEST_SKIP() << "Skipping network test (TF_NETWORK_TESTS != 1). "
                        "Set TF_NETWORK_TESTS=1 to enable.";
    }

    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://example.com/";
    req.timeout = std::chrono::milliseconds{15'000};

    auto result = client.send(req);

    ASSERT_TRUE(result.has_value()) << "Transport failure: send() returned nullopt";
    EXPECT_GE(result->status_code, 200L);
    EXPECT_LT(result->status_code, 300L)
        << "Expected 2xx, got " << result->status_code;
    EXPECT_FALSE(result->body.empty())
        << "Expected non-empty response body from example.com";
}

// Verify response headers are captured.
TEST(CurlHttpClientTest, NetworkResponseHeadersCaptured) {
    if (!network_tests_enabled()) {
        GTEST_SKIP() << "Skipping network test (TF_NETWORK_TESTS != 1). "
                        "Set TF_NETWORK_TESTS=1 to enable.";
    }

    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://example.com/";
    req.timeout = std::chrono::milliseconds{15'000};

    auto result = client.send(req);

    ASSERT_TRUE(result.has_value()) << "Transport failure";
    // example.com always sends Content-Type
    EXPECT_FALSE(result->headers.empty())
        << "Expected at least one response header";
}

// --------------------------------------------------------------------------
// CRLF-injection guard (Phase 0.B deferred).
//
// A caller-supplied header containing "\r\n" is HTTP header injection
// (smuggling).  send() must reject it before any network I/O.  We use the
// guaranteed-unreachable test-net URL so we know nothing went over the
// wire: if validation fired, send() returns nullopt without ever building
// a connection.
// --------------------------------------------------------------------------
TEST(CurlHttpClientTest, RejectsCRLFInHeaderValue) {
    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://192.0.2.1/";          // TEST-NET-1
    req.timeout = std::chrono::milliseconds{3'000};
    req.headers["X-Evil"] = "value\r\nX-Injected: malicious";

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value())
        << "send() must reject a header value containing CRLF";
}

TEST(CurlHttpClientTest, RejectsCRLFInHeaderName) {
    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://192.0.2.1/";
    req.timeout = std::chrono::milliseconds{3'000};
    req.headers["X-Evil\r\nX-Injected"] = "value";

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value())
        << "send() must reject a header name containing CRLF";
}

TEST(CurlHttpClientTest, RejectsLoneNewlineInHeader) {
    CurlHttpClient client;
    HttpRequest req;
    req.method  = "GET";
    req.url     = "https://192.0.2.1/";
    req.timeout = std::chrono::milliseconds{3'000};
    req.headers["X-Evil"] = "v\nX-Injected: x";

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value())
        << "send() must reject a bare '\\n' in a header value too";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
