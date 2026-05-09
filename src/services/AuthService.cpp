// AuthService.cpp — TUI-side authentication service (Phase 3.B).
//
// See AuthService.h for the full contract.
//
// SECRETS HYGIENE:
//   - Session tokens, passphrases, and TOTP codes are NEVER passed to Logger.
//   - Local token copies (std::string) are zeroed with sodium_memzero after use.
//   - The secret store holds an opaque blob; its lifecycle is managed by
//     cache_token() / clear_cached_token().

#include "AuthService.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <span>

using json = nlohmann::json;

// --------------------------------------------------------------------------
// Helpers — zeroize a std::string in place
// --------------------------------------------------------------------------

static void zeroize_string(std::string& s) {
    if (!s.empty()) {
        sodium_memzero(s.data(), s.size());
    }
    s.clear();
}

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

AuthService::AuthService(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets,
    std::string                    user_email)
    : backend_(std::move(backend))
    , secrets_(std::move(secrets))
    , email_(std::move(user_email))
{
}

// --------------------------------------------------------------------------
// Public API — login
// --------------------------------------------------------------------------

std::variant<LoginResult, BackendError> AuthService::login(const LoginRequest& req) {
    // Build request body. Never log req.passphrase or req.totp_code.
    json body;
    body["email"]     = req.email;
    body["passphrase"] = req.passphrase;
    body["totp_code"]  = req.totp_code;

    auto result = backend_->post("/auth/login", body);

    if (std::holds_alternative<BackendError>(result)) {
        Logger::instance().info("AuthService::login failed");
        return std::get<BackendError>(result);
    }

    const auto& j = std::get<json>(result);

    // Extract required fields from the response.
    if (!j.contains("session_token") || !j.contains("user_id") || !j.contains("expires_at")) {
        return BackendError{
            BackendError::Kind::BadResponse,
            200,
            "missing_fields",
            "login response missing required fields"
        };
    }

    LoginResult lr;
    lr.session_token  = j["session_token"].get<std::string>();
    lr.user_id        = j["user_id"].get<std::string>();
    lr.expires_at_unix = j["expires_at"].get<int64_t>();

    // Cache the session token in the secret store.
    cache_token(lr.session_token);

    Logger::instance().info("AuthService::login succeeded for user: " + lr.user_id);
    return lr;
}

// --------------------------------------------------------------------------
// Public API — enroll
// --------------------------------------------------------------------------

std::variant<EnrollResult, BackendError> AuthService::enroll(const EnrollRequest& req) {
    // Build request body. Never log req.passphrase.
    json body;
    body["token"]     = req.token;
    body["email"]     = req.email;
    body["passphrase"] = req.passphrase;

    if (req.totp_secret.has_value()) {
        // Encode the raw TOTP secret bytes as hex for transport.
        const auto& bytes = req.totp_secret.value();
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (auto b : bytes) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(b));
            hex += buf;
        }
        body["totp_secret_hex"] = hex;
        zeroize_string(hex);
    }

    auto result = backend_->post("/auth/enroll", body);

    if (std::holds_alternative<BackendError>(result)) {
        Logger::instance().info("AuthService::enroll failed");
        return std::get<BackendError>(result);
    }

    const auto& j = std::get<json>(result);

    if (!j.contains("user_id") || !j.contains("totp_provisioning_uri")) {
        return BackendError{
            BackendError::Kind::BadResponse,
            200,
            "missing_fields",
            "enroll response missing required fields"
        };
    }

    EnrollResult er;
    er.user_id               = j["user_id"].get<std::string>();
    er.totp_provisioning_uri = j["totp_provisioning_uri"].get<std::string>();

    Logger::instance().info("AuthService::enroll succeeded; call login() next");
    // Do NOT cache any session token — enroll does not produce one.
    return er;
}

// --------------------------------------------------------------------------
// Public API — logout
// --------------------------------------------------------------------------

bool AuthService::logout() {
    auto token_opt = read_cached_token();
    if (!token_opt.has_value()) {
        Logger::instance().info("AuthService::logout: no cached session token");
        return true; // nothing to revoke
    }

    std::string token = std::move(*token_opt);

    json body = json::object(); // empty body; token is in the Authorization header
    auto result = backend_->post("/auth/logout", body, token);

    // Zeroize the local token copy regardless of outcome.
    zeroize_string(token);

    if (std::holds_alternative<BackendError>(result)) {
        const auto& err = std::get<BackendError>(result);
        if (err.kind == BackendError::Kind::Transport) {
            // Transport failure — don't clear the cache, the token might still be valid.
            Logger::instance().warning("AuthService::logout: transport failure; cache unchanged");
            return false;
        }
        // Any other server-side error (4xx / 5xx) — consider the session gone; clear cache.
        clear_cached_token();
        return false;
    }

    // 2xx — successful revocation.
    clear_cached_token();
    Logger::instance().info("AuthService::logout: session revoked");
    return true;
}

// --------------------------------------------------------------------------
// Public API — current_user_id
// --------------------------------------------------------------------------

std::optional<std::string> AuthService::current_user_id() {
    auto token_opt = read_cached_token();
    if (!token_opt.has_value()) {
        // No cached session — skip the network call entirely.
        return std::nullopt;
    }

    std::string token = std::move(*token_opt);

    auto result = backend_->get("/auth/whoami", token);

    // Zeroize the local copy of the token.
    zeroize_string(token);

    if (std::holds_alternative<BackendError>(result)) {
        const auto& err = std::get<BackendError>(result);
        if (err.kind == BackendError::Kind::Unauthorized) {
            // 401: token is stale — remove it from the cache automatically.
            Logger::instance().info("AuthService::current_user_id: session expired; clearing cache");
            clear_cached_token();
        } else {
            Logger::instance().info("AuthService::current_user_id: request failed");
        }
        return std::nullopt;
    }

    const auto& j = std::get<json>(result);
    if (!j.contains("user_id")) {
        return std::nullopt;
    }

    return j["user_id"].get<std::string>();
}

// --------------------------------------------------------------------------
// Public API — has_cached_session
// --------------------------------------------------------------------------

bool AuthService::has_cached_session() {
    return secrets_->get(cache_key()).has_value();
}

// --------------------------------------------------------------------------
// Private helpers
// --------------------------------------------------------------------------

std::string AuthService::cache_key() const {
    return "tf-session-" + email_;
}

std::optional<std::string> AuthService::read_cached_token() {
    auto bytes_opt = secrets_->get(cache_key());
    if (!bytes_opt.has_value()) {
        return std::nullopt;
    }

    const auto& bytes = *bytes_opt;
    // The session token is opaque ASCII (base64url); convert bytes to string.
    std::string token(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return token;
}

void AuthService::cache_token(const std::string& token) {
    // Store the token as raw bytes in the secret store.
    const auto* ptr = reinterpret_cast<const std::byte*>(token.data());
    std::span<const std::byte> span{ptr, token.size()};
    if (!secrets_->put(cache_key(), span)) {
        Logger::instance().warning("AuthService: failed to cache session token in secret store");
    }
}

void AuthService::clear_cached_token() {
    secrets_->remove(cache_key());
}
