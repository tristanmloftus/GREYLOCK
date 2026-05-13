#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "IHttpClient.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SyncStatusItem — per-Plaid-Item sync record returned by GET /sync-status.
// ---------------------------------------------------------------------------
// Task v0.3-3.  One entry per linked Plaid Item.  The TUI's Drill_SyncStatus
// view renders these rows when the endpoint is available; when the server
// returns 404 (endpoint not yet wired — v0.4 server work), the drill falls
// back to the DataStore-derived view used by the dashboard widget today.
//
// FIELD SEMANTICS (mirrors the v0.3 UI mockup in docs/UI_REDESIGN_V0.3.md
// §3b "Drill 3 — Sync Status"):
//   institution         Human-readable institution name ("Chase").
//   item_id             Plaid Item ID; the audit-log/refresh key.
//   last_success_unix   Unix seconds of the most recent successful sync.
//                       0 sentinel == never successful.
//   last_attempt_unix   Unix seconds of the most recent sync attempt
//                       (success or failure).  0 sentinel == never tried.
//   last_error_code     Empty string on success; otherwise the Plaid /
//                       server error code (e.g. "ITEM_LOGIN_REQUIRED",
//                       "AUTH_ERROR").  The drill view uses this string
//                       to decide whether the [R] re-auth key is enabled.
//   account_ids         The DataStore account.id list this Item owns; the
//                       drill cross-references these to render the per-
//                       account masked-id table inside the Item block.
// ---------------------------------------------------------------------------
struct SyncStatusItem {
    std::string              institution;
    std::string              item_id;
    int64_t                  last_success_unix = 0;
    int64_t                  last_attempt_unix = 0;
    std::string              last_error_code;
    std::vector<std::string> account_ids;
};

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

    // -------------------------------------------------------------------
    // get_sync_status — call GET /sync-status, parse the per-Item array.
    // -------------------------------------------------------------------
    // Task v0.3-3.  Returns std::nullopt when the server responds 404
    // (endpoint not yet implemented — v0.4 server work).  The Drill_
    // SyncStatus view treats nullopt as "fall back to the DataStore-
    // derived view the dashboard widget already renders" and gates the
    // [R] re-auth key off — see docs/UI_REDESIGN_V0.3.md § Task v0.3-3.
    //
    // Returns the parsed vector on a 200-with-valid-JSON response.  Other
    // failures (transport / 5xx / malformed body) propagate as a
    // BackendError so the drill view can show "server endpoint required
    // to re-auth — coming v0.4." in the UI without conflating "endpoint
    // missing" with "endpoint broken".
    //
    // JSON schema this method expects (v0.4 server contract):
    //   {
    //     "items": [
    //       { "institution": "Chase",
    //         "item_id":     "item_abc123",
    //         "last_success_unix": 1712001234,
    //         "last_attempt_unix": 1712001234,
    //         "last_error_code":   "",
    //         "account_ids":       ["acct-1234", "acct-5678"] },
    //       ...
    //     ]
    //   }
    //
    // SECURITY: the session token is forwarded as a Bearer header so the
    // server can scope rows to the logged-in user.  Endpoint should
    // return 401 when the token is missing/invalid — surfaced as a
    // BackendError with Kind::Unauthorized.
    std::variant<std::optional<std::vector<SyncStatusItem>>, BackendError>
    get_sync_status(std::optional<std::string> session_token = std::nullopt);

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
