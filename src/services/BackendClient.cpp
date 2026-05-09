#include "BackendClient.h"
#include "../utils/Logger.h"

#include <stdexcept>

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

BackendClient::BackendClient(std::shared_ptr<IHttpClient> http, std::string base_url)
    : http_(std::move(http))
    , base_url_(std::move(base_url))
{
    // F-2 compliance: reject any non-HTTPS base URL explicitly.
    // We do NOT silently accept http:// — that would violate the security contract.
    if (base_url_.rfind("https://", 0) != 0) {
        throw std::invalid_argument(
            "BackendClient: base_url must start with \"https://\". Got: \"" +
            base_url_ + "\"");
    }
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

std::variant<json, BackendError> BackendClient::get(
    std::string_view path,
    std::optional<std::string> session_token)
{
    return send_request("GET", path, std::nullopt, session_token);
}

std::variant<json, BackendError> BackendClient::post(
    std::string_view path,
    const json& body,
    std::optional<std::string> session_token)
{
    return send_request("POST", path, body.dump(), session_token);
}

std::variant<bool, BackendError> BackendClient::healthz() {
    auto result = get("/healthz");
    if (std::holds_alternative<json>(result)) {
        return true;
    }
    return std::get<BackendError>(result);
}

// --------------------------------------------------------------------------
// Private helpers
// --------------------------------------------------------------------------

std::variant<json, BackendError> BackendClient::send_request(
    const std::string& method,
    std::string_view path,
    const std::optional<std::string>& body_json,
    const std::optional<std::string>& session_token)
{
    HttpRequest req;
    req.method = method;
    req.url    = base_url_ + std::string(path);
    req.headers["Content-Type"] = "application/json";

    if (session_token.has_value()) {
        req.headers["Authorization"] = "Bearer " + *session_token;
    }

    if (body_json.has_value()) {
        req.body = *body_json;
    }

    auto response = http_->send(req);

    if (!response.has_value()) {
        Logger::instance().warning(
            "BackendClient: transport failure [" + method + " " + req.url + "]");
        return BackendError{
            BackendError::Kind::Transport,
            0,
            "transport_failure",
            "request did not complete"
        };
    }

    return map_response(*response);
}

std::variant<json, BackendError> BackendClient::map_response(const HttpResponse& resp) {
    const long status = resp.status_code;

    // 2xx: attempt JSON parse.
    if (status >= 200 && status < 300) {
        json parsed = json::parse(resp.body, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            return BackendError{
                BackendError::Kind::BadResponse,
                status,
                "invalid_json",
                "2xx response body is not valid JSON: " + resp.body
            };
        }
        return parsed;
    }

    // Specific 4xx mappings.
    if (status == 401) {
        return BackendError{BackendError::Kind::Unauthorized, status, "unauthorized", "unauthorized"};
    }
    if (status == 404) {
        return BackendError{BackendError::Kind::NotFound, status, "not_found", "not found"};
    }
    if (status == 409) {
        return BackendError{BackendError::Kind::Conflict, status, "conflict", "conflict"};
    }
    if (status == 429) {
        return BackendError{BackendError::Kind::RateLimited, status, "rate_limited", "rate limited"};
    }

    // 5xx → ServerError.
    if (status >= 500 && status < 600) {
        // Try to extract code/message from JSON error body.
        json err_body = json::parse(resp.body, nullptr, /*allow_exceptions=*/false);
        std::string code    = "server_error";
        std::string message = "server error";
        if (!err_body.is_discarded()) {
            if (err_body.contains("code") && err_body["code"].is_string()) {
                code = err_body["code"].get<std::string>();
            }
            if (err_body.contains("message") && err_body["message"].is_string()) {
                message = err_body["message"].get<std::string>();
            }
        }
        return BackendError{BackendError::Kind::ServerError, status, code, message};
    }

    // Other 4xx → BadResponse; parse code/message from JSON body if present.
    {
        json err_body = json::parse(resp.body, nullptr, /*allow_exceptions=*/false);
        std::string code    = "bad_request";
        std::string message = "bad request";
        if (!err_body.is_discarded()) {
            if (err_body.contains("code") && err_body["code"].is_string()) {
                code = err_body["code"].get<std::string>();
            }
            if (err_body.contains("message") && err_body["message"].is_string()) {
                message = err_body["message"].get<std::string>();
            }
        }
        return BackendError{BackendError::Kind::BadResponse, status, code, message};
    }
}
