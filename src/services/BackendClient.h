#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "IHttpClient.h"

using json = nlohmann::json;

// Structured error returned by BackendClient on any non-2xx-with-valid-JSON outcome.
struct BackendError {
    enum class Kind {
        Transport,      // IHttpClient returned nullopt (TLS error, connect failure, timeout)
        BadResponse,    // 2xx but invalid JSON body, OR other 4xx with no recognised code
        ServerError,    // 5xx
        Unauthorized,   // 401
        NotFound,       // 404
        Conflict,       // 409
        RateLimited,    // 429
    };

    Kind        kind;
    long        http_status;  // 0 for Transport errors; actual status code otherwise
    std::string code;         // machine-readable error code ("transport_failure", "invalid_json", …)
    std::string message;      // human-readable description
};

// TUI-side client that wraps IHttpClient, knows the backend base URL, and maps
// HTTP/transport errors to structured BackendError values.
//
// URL composition: base_url + path.  base_url must start with "https://" —
// the constructor throws std::invalid_argument for non-HTTPS URLs (F-2 compliance).
//
// Session token: when present, "Authorization: Bearer <token>" is set on every
// outgoing request.  Phase 2 always passes nullopt; Phase 3 will populate it.
//
// The BackendClient is a thin wrapper.  It does not retry, rate-limit, or sign
// requests — those are Phase 3+ concerns.
class BackendClient {
public:
    // Constructs a BackendClient.
    // Throws std::invalid_argument if base_url does not start with "https://".
    BackendClient(std::shared_ptr<IHttpClient> http, std::string base_url);

    // Send GET <base_url + path>.
    // Returns parsed nlohmann::json on success (2xx with valid JSON body).
    // Returns BackendError on any failure.
    std::variant<json, BackendError> get(
        std::string_view path,
        std::optional<std::string> session_token = std::nullopt);

    // Send POST <base_url + path> with JSON body.
    // Returns parsed nlohmann::json on success.
    // Returns BackendError on any failure.
    std::variant<json, BackendError> post(
        std::string_view path,
        const json& body,
        std::optional<std::string> session_token = std::nullopt);

    // Convenience: call GET /healthz, return true on success.
    // Returns false-equivalent BackendError on failure.
    std::variant<bool, BackendError> healthz();

private:
    // Shared helper: build HttpRequest, call send(), map response.
    std::variant<json, BackendError> send_request(
        const std::string& method,
        std::string_view path,
        const std::optional<std::string>& body_json,
        const std::optional<std::string>& session_token);

    // Map an HttpResponse to a json/BackendError variant.
    std::variant<json, BackendError> map_response(const HttpResponse& resp);

    std::shared_ptr<IHttpClient> http_;
    std::string base_url_;
};
