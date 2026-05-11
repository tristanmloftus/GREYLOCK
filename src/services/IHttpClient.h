#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

// An HTTP request. Callers fill this in and pass to IHttpClient::send().
// TLS verification is always ON in the implementation — there is no insecure flag.
struct HttpRequest {
    std::string method;                          // "GET", "POST", "PUT", "DELETE"
    std::string url;                             // Full URL including scheme, host, path, query
    std::map<std::string, std::string> headers;  // Additional request headers
    std::optional<std::string> body;             // Request body (POST/PUT)
    std::chrono::milliseconds timeout{30'000};   // Per-request timeout (connect + transfer combined)

    // Optional path to a PEM CA bundle file.  When set, the implementation
    // uses this file as the sole trust anchor for TLS verification instead of
    // the system/libcurl default bundle.  This is the ONLY supported
    // mechanism for testing against a custom CA (e.g. mkcert dev certs in CI).
    //
    // TLS verification remains ON regardless of which CA bundle is used.
    // Setting this field to an invalid path causes send() to return nullopt
    // (libcurl's CURLOPT_CAINFO honours the same contract).
    //
    // Phase 2 use-case: integration tests need to point the CurlHttpClient at
    // the mkcert rootCA.pem (good case) or at an unrelated CA (bad case, to
    // prove that TLS verify actually runs).  Production code (main.cpp,
    // BackendClient) leaves this field unset — the system bundle is correct
    // for Let's Encrypt certs in Phase 6 deploy.
    std::optional<std::string> ca_bundle_path;  // nullopt = use libcurl default
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
