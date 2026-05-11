#pragma once

// PlaidTokenBroker.h — Server-side broker for Plaid access tokens.
//
// DESIGN:
//   Plaid access tokens are NEVER held plaintext outside a single broker scope.
//   The broker:
//     1. Derives a per-account DEK from the master key via libsodium KDF.
//        The master key is CONSUMED in the constructor: copied into
//        master_key_, fed to derive_dek(), then immediately zeroed via
//        sodium_memzero.  Only the derived DEK survives for the broker's
//        lifetime — this minimizes the master-key exposure window against
//        memory-disclosure attacks (core dumps, swap, ptrace).
//     2. Uses EnvelopeEncryption (XChaCha20-Poly1305) with AAD=account_id_bytes
//        so a token cannot be moved to another account (AAD-binding).
//     3. Exposes withDecryptedToken<F>() so callers get a std::string_view over
//        a ZeroizingBuffer that is destroyed (zeroed) when the scope exits.
//
// MASTER KEY (F-1):
//   Read from the TF_MASTER_KEY environment variable at construction time.
//   The env var must contain exactly 32 hex-encoded bytes (64 hex chars) or
//   exactly 32 raw bytes.  If 4.E (SQLCipher) has shipped its master-key
//   wiring, the broker can be constructed with the master key passed in
//   directly (the alternative constructor) so main.cpp wires it once.
//
//   ASSUMPTION: If 4.E has not shipped, the master key is sourced here from
//   TF_MASTER_KEY.  When 4.E ships, main.cpp should call the constructor that
//   accepts a master_key parameter instead, so the key is read once.
//
// DEK DERIVATION:
//   A single shared DEK is derived for all Plaid tokens:
//     crypto_kdf_derive_from_key(dek, 32, subkey_id=1, ctx="tf-plaid", master_key)
//   This is simpler than per-account DEKs and sufficient for the v0.2 threat
//   model where the master key is the ultimate secret.  The AAD=account_id
//   binding on the AEAD layer prevents ciphertext from being moved between
//   accounts even when all tokens share the same DEK.
//
//   (Per-account DEKs would require a stable subkey_id for each account, e.g.
//    derived from account_id via a hash — feasible but adds complexity.  The
//    shared-DEK approach is documented here so the choice is explicit.)
//
// SCHEMA:
//   Uses the accounts table (M001):
//     encrypted_token  BLOB    — stores the EnvelopeEncryption blob
//     is_plaid_linked  INTEGER — 1 when a token is present
//
// GUARDRAILS:
//   F-1: Master key from env var, NOT from any caller-supplied input.
//   F-2: withDecryptedToken always ACTUALLY decrypts; "no token" path does
//        NOT return a empty string_view claiming to be a token.
//   F-3: Plaintext token is NEVER logged (not even at debug level).
//   F-4: Token bytes live only inside ZeroizingBuffer; the callable receives
//        a std::string_view over that buffer.  On scope exit, zeroed.

#include "../../server/db/Database.h"
#include "../../src/services/crypto/EnvelopeEncryption.h"
#include "../../src/services/crypto/Zeroize.h"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <sodium.h>

namespace tf::plaid {

// ---------------------------------------------------------------------------
// NoTokenTag — sentinel type returned by withDecryptedToken when no encrypted
// token exists for the account.  Forces callers to handle the missing-token
// case explicitly rather than receiving an empty string_view.
// ---------------------------------------------------------------------------
struct NoTokenTag {};

// ---------------------------------------------------------------------------
// PlaidTokenBroker
//
// Construction: pass the master key bytes directly (preferred) or call
// PlaidTokenBroker(db) to read from TF_MASTER_KEY env var.
//
// Thread safety: NOT thread-safe.  External synchronization required.
// ---------------------------------------------------------------------------
class PlaidTokenBroker {
public:
    // Construct from a 32-byte master key (e.g. from main.cpp after 4.E ships).
    explicit PlaidTokenBroker(
        Database& db,
        std::span<const std::byte, crypto_kdf_KEYBYTES> master_key
    );

    // Construct by reading TF_MASTER_KEY from the environment (hex or raw).
    // Throws std::runtime_error if the env var is absent or malformed.
    explicit PlaidTokenBroker(Database& db);

    // Not copyable or movable (holds a reference to Database).
    PlaidTokenBroker(const PlaidTokenBroker&) = delete;
    PlaidTokenBroker& operator=(const PlaidTokenBroker&) = delete;
    PlaidTokenBroker(PlaidTokenBroker&&) = delete;
    PlaidTokenBroker& operator=(PlaidTokenBroker&&) = delete;

    ~PlaidTokenBroker();

    // -----------------------------------------------------------------------
    // store_token(account_id, plaintext_token)
    //
    // Encrypts `plaintext_token` with EnvelopeEncryption (AAD=account_id bytes)
    // and persists the blob + sets is_plaid_linked=1.
    //
    // The plaintext_token span is read, then zeroed by the caller — this
    // function does NOT zero it (it does not own the buffer).
    // -----------------------------------------------------------------------
    void store_token(const std::string& account_id,
                     std::span<const std::byte> plaintext_token);

    // Convenience overload: accepts the token as a std::string.
    // The string is read and NOT modified; caller is responsible for zeroing
    // if needed (e.g. via sodium_memzero on the string's data).
    void store_token(const std::string& account_id,
                     const std::string& plaintext_token);

    // -----------------------------------------------------------------------
    // clear_token(account_id)
    //
    // Sets encrypted_token=NULL, is_plaid_linked=0 for the given account.
    // No-op if the account does not exist or has no token.
    // -----------------------------------------------------------------------
    void clear_token(const std::string& account_id);

    // -----------------------------------------------------------------------
    // withDecryptedToken<F>(account_id, f) -> WithDecryptedTokenResult<F>
    //
    // F must be callable as:
    //   - f(std::string_view token)  → some return type R (token present path)
    // Additionally, F must provide an overload or be invocable with NoTokenTag:
    //   - f(NoTokenTag)              → R                 (no token path)
    //
    // Alternatively, use the two-callable variant withDecryptedToken(id, on_token, on_no_token).
    //
    // SECURITY:
    //   The plaintext token exists only within the scope of f().
    //   Once f() returns, the ZeroizingBuffer is destroyed and bytes are zeroed.
    //   NEVER escape the string_view beyond f()'s scope.
    //
    // F-2: If encrypted_token is NULL or empty, this calls f(NoTokenTag{})
    //      and does NOT call f with an empty string_view.
    // -----------------------------------------------------------------------

    // Single-callable variant: F must accept both std::string_view and NoTokenTag.
    template <typename F>
    auto withDecryptedToken(const std::string& account_id, F&& f)
        -> std::invoke_result_t<F, std::string_view>
    {
        static_assert(
            std::is_invocable_v<F, NoTokenTag>,
            "withDecryptedToken<F>: F must be callable with NoTokenTag for the no-token path. "
            "Use the two-callable variant or add an overload for NoTokenTag.");

        auto blob_opt = fetch_encrypted_blob(account_id);
        if (!blob_opt.has_value()) {
            return std::forward<F>(f)(NoTokenTag{});
        }

        return decrypt_and_invoke(account_id, *blob_opt, std::forward<F>(f));
    }

    // Two-callable variant: cleaner API when on_token and on_no_token have
    // different logic.
    template <typename OnToken, typename OnNoToken>
    auto withDecryptedToken(const std::string& account_id,
                             OnToken&& on_token,
                             OnNoToken&& on_no_token)
        -> std::invoke_result_t<OnToken, std::string_view>
    {
        static_assert(
            std::is_same_v<
                std::invoke_result_t<OnToken, std::string_view>,
                std::invoke_result_t<OnNoToken>>,
            "withDecryptedToken: on_token and on_no_token must return the same type.");

        auto blob_opt = fetch_encrypted_blob(account_id);
        if (!blob_opt.has_value()) {
            return std::forward<OnNoToken>(on_no_token)();
        }

        return decrypt_and_invoke(account_id, *blob_opt, std::forward<OnToken>(on_token));
    }

private:
    // Derive the DEK from the master key using libsodium KDF.
    static tf::crypto::EnvelopeKey derive_dek(
        const std::array<std::byte, crypto_kdf_KEYBYTES>& master_key);

    // SELECT encrypted_token FROM accounts WHERE id = account_id.
    // Returns nullopt if no token exists (NULL column or no such account).
    std::optional<std::vector<std::byte>> fetch_encrypted_blob(
        const std::string& account_id);

    // Decrypt blob and invoke callable with a string_view over the plaintext.
    // The plaintext is zeroed before returning.
    template <typename F>
    auto decrypt_and_invoke(
        const std::string& account_id,
        const std::vector<std::byte>& blob,
        F&& f) -> std::invoke_result_t<F, std::string_view>
    {
        // Build AAD from account_id bytes.
        auto aad = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(account_id.data()),
            account_id.size());

        auto plaintext_opt = tf::crypto::decrypt(blob, aad, dek_);
        if (!plaintext_opt.has_value()) {
            throw std::runtime_error(
                "PlaidTokenBroker: decryption failed for account " + account_id +
                " — tag mismatch or corrupted blob.");
        }

        // Move plaintext into a ZeroizingBuffer so it is zeroed on scope exit.
        tf::crypto::ZeroizingBuffer plaintext_buf(
            std::span<const std::byte>(plaintext_opt->data(), plaintext_opt->size()));

        // Explicitly zero the vector returned by decrypt before it destructs.
        sodium_memzero(plaintext_opt->data(), plaintext_opt->size());

        // Construct string_view over the ZeroizingBuffer.
        std::string_view token_view(
            reinterpret_cast<const char*>(plaintext_buf.data()),
            plaintext_buf.size());

        // Invoke the callable with the string_view.
        // ZeroizingBuffer destructs after f() returns — zeroed on scope exit.
        return std::forward<F>(f)(token_view);
        // ^^^ plaintext_buf zeroed here (destructor).
    }

    Database& db_;
    tf::crypto::EnvelopeKey dek_;  // Derived from master key; zeroed in destructor.
    // Zeroed immediately after DEK derivation in constructor; destructor
    // zeros again as belt-and-suspenders.  Never read after construction.
    std::array<std::byte, crypto_kdf_KEYBYTES> master_key_;
};

} // namespace tf::plaid
