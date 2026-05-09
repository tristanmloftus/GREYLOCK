#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

using namespace std::chrono_literals;

// An HTTP request. Callers fill this in and pass to IHttpClient::send().
// TLS verification is always ON in the implementation — there is no insecure flag.
struct HttpRequest {
    std::string method;                          // "GET", "POST", "PUT", "DELETE"
    std::string url;                             // Full URL including scheme, host, path, query
    std::map<std::string, std::string> headers;  // Additional request headers
    std::optional<std::string> body;             // Request body (POST/PUT)
    std::chrono::milliseconds timeout{30'000};   // Per-request timeout (connect + transfer combined)
};

// An HTTP response as returned by IHttpClient::send().
struct HttpResponse {
    long status_code{0};
    std::map<std::string, std::string> headers;  // Response headers (case-preserving for Phase 0)
    std::string body;
};

// Abstract HTTP client interface. Implementations:
//   - CurlHttpClient  (libcurl; see services/http/CurlHttpClient.h)
//   - FakeHttpClient  (Phase 1 unit-test double; not in Phase 0)
//
// Contract:
//   - send() returns std::nullopt on network/transport error (DNS failure, connect timeout,
//     TLS handshake failure). A non-2xx HTTP status is NOT a transport error — the caller
//     inspects HttpResponse::status_code.
//   - TLS verification is always enforced. Implementations must not expose a bypass.
//   - Requests must not hang forever — every call is bounded by req.timeout.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    // Execute the request. Returns std::nullopt on transport-layer failure.
    virtual std::optional<HttpResponse> send(const HttpRequest& req) = 0;
};
