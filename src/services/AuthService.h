#pragma once

// AuthService.h — TUI-side authentication service (Phase 3.B).
//
// Wraps BackendClient with login/logout/enroll/whoami operations.
// Caches the opaque session token in the OS secret store via ISecretStore.
//
// Session token key format: "tf-session-{email}"
// The token is an opaque ASCII string (base64url) from the server.
//
// SECRETS HYGIENE:
//   - Session tokens are never logged.
//   - Passphrases and TOTP codes are never logged.
//   - Local token copies are zeroed via sodium_memzero after use.
//   - Cache is cleared automatically on 401 (stale token).

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "BackendClient.h"
#include "ISecretStore.h"

// --------------------------------------------------------------------------
// Request / result types
// --------------------------------------------------------------------------

struct LoginRequest {
    std::string email;
    std::string passphrase;
    std::string totp_code;
};

struct EnrollRequest {
    std::string token;      // invite/enrollment token from the server operator
    std::string email;
    std::string passphrase;
    std::optional<std::vector<std::byte>> totp_secret; // raw TOTP secret bytes if pre-generated
};

struct EnrollResult {
    std::string user_id;
    std::string totp_provisioning_uri;
};

struct LoginResult {
    std::string user_id;
    std::string session_token;
    int64_t     expires_at_unix;  // Unix timestamp when the session expires
};

// --------------------------------------------------------------------------
// AuthService
// --------------------------------------------------------------------------

class AuthService {
public:
    // Construct with a BackendClient, a secret store, and the user's email
    // address (used as the key suffix for the session token cache).
    // email comes from TF_USER_EMAIL env var or is provided at construction.
    AuthService(
        std::shared_ptr<BackendClient> backend,
        std::shared_ptr<ISecretStore>  secrets,
        std::string                    user_email);

    // POST /auth/login
    // On success: caches session_token in ISecretStore under "tf-session-{email}".
    // On failure: secret store is not modified.
    std::variant<LoginResult, BackendError> login(const LoginRequest& req);

    // POST /auth/enroll
    // Does NOT cache any credential; caller must invoke login() afterward.
    std::variant<EnrollResult, BackendError> enroll(const EnrollRequest& req);

    // POST /auth/logout (authenticated)
    // Reads cached session token, sends revocation, then removes cache entry.
    // Returns true on successful revocation (HTTP 2xx).
    // On transport failure: returns false without clearing the cache —
    // the token may still be valid on the server.
    bool logout();

    // GET /auth/whoami (authenticated)
    // Returns user_id string on 200.
    // Returns nullopt on 401 (expired/revoked) or transport failure.
    // On 401: AUTOMATICALLY removes the cached session token (it is stale).
    std::optional<std::string> current_user_id();

    // Non-network check: returns true iff ISecretStore has a cached session token.
    bool has_cached_session();

private:
    // Read the cached session token from the secret store.
    // Returns nullopt if no token is cached.
    std::optional<std::string> read_cached_token();

    // Write a session token to the secret store.
    void cache_token(const std::string& token);

    // Remove the cached session token from the secret store.
    void clear_cached_token();

    // The cache key used for this user's session token.
    std::string cache_key() const;

    std::shared_ptr<BackendClient> backend_;
    std::shared_ptr<ISecretStore>  secrets_;
    std::string                    email_;
};
