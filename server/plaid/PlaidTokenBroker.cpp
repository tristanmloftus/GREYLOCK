// PlaidTokenBroker.cpp — Server-side broker for Plaid access tokens.
//
// See PlaidTokenBroker.h for design notes, guardrails, and security contract.

#include "PlaidTokenBroker.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <sodium.h>

namespace tf::plaid {

// ---------------------------------------------------------------------------
// KDF context string for Plaid token DEK derivation.
// Must be exactly 8 bytes (crypto_kdf_CONTEXTBYTES = 8).
// "tf-plaid" is 8 bytes.
// ---------------------------------------------------------------------------
static constexpr char KDF_CTX[crypto_kdf_CONTEXTBYTES] = {
    't', 'f', '-', 'p', 'l', 'a', 'i', 'd'
};
static constexpr uint64_t PLAID_DEK_SUBKEY_ID = 1;

// ---------------------------------------------------------------------------
// derive_dek — static helper
// ---------------------------------------------------------------------------
tf::crypto::EnvelopeKey PlaidTokenBroker::derive_dek(
    const std::array<std::byte, crypto_kdf_KEYBYTES>& master_key)
{
    std::array<std::byte, tf::crypto::EnvelopeKey::KEY_BYTES> dek_bytes{};

    int rc = crypto_kdf_derive_from_key(
        reinterpret_cast<unsigned char*>(dek_bytes.data()),
        dek_bytes.size(),
        PLAID_DEK_SUBKEY_ID,
        KDF_CTX,
        reinterpret_cast<const unsigned char*>(master_key.data())
    );

    if (rc != 0) {
        sodium_memzero(dek_bytes.data(), dek_bytes.size());
        throw std::runtime_error(
            "PlaidTokenBroker: crypto_kdf_derive_from_key failed");
    }

    auto dek_span = std::span<const std::byte, tf::crypto::EnvelopeKey::KEY_BYTES>{dek_bytes};
    tf::crypto::EnvelopeKey key{dek_span};
    sodium_memzero(dek_bytes.data(), dek_bytes.size());
    return key;
}

// ---------------------------------------------------------------------------
// Constructor — direct master key (preferred; used by main.cpp after 4.E ships)
// ---------------------------------------------------------------------------
PlaidTokenBroker::PlaidTokenBroker(
    Database& db,
    std::span<const std::byte, crypto_kdf_KEYBYTES> master_key)
    : db_(db)
    , master_key_{}
{
    if (sodium_init() < 0) {
        throw std::runtime_error("PlaidTokenBroker: sodium_init() failed");
    }
    std::copy(master_key.begin(), master_key.end(), master_key_.begin());
    dek_ = derive_dek(master_key_);
    // master_key_ is kept in case we need to re-derive; it is zeroed in the destructor.
}

// ---------------------------------------------------------------------------
// Constructor — reads TF_MASTER_KEY from environment.
//
// Accepted formats:
//   64 hex characters  → decoded to 32 bytes
//   32 raw bytes       → used directly (length-gated via env var convention)
//
// Throws std::runtime_error if TF_MASTER_KEY is absent, wrong length, or
// contains non-hex characters when 64 chars are supplied.
// ---------------------------------------------------------------------------
PlaidTokenBroker::PlaidTokenBroker(Database& db)
    : db_(db)
    , master_key_{}
{
    if (sodium_init() < 0) {
        throw std::runtime_error("PlaidTokenBroker: sodium_init() failed");
    }

    const char* env_val = std::getenv("TF_MASTER_KEY");
    if (!env_val || env_val[0] == '\0') {
        throw std::runtime_error(
            "PlaidTokenBroker: TF_MASTER_KEY environment variable is not set. "
            "Set it to a 64-hex-character (32-byte) master key.");
    }

    std::string key_str(env_val);

    if (key_str.size() == 64) {
        // Hex-encoded: decode 2 chars at a time.
        for (std::size_t i = 0; i < 32; ++i) {
            char hi = key_str[2 * i];
            char lo = key_str[2 * i + 1];
            auto hex_digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hv = hex_digit(hi);
            int lv = hex_digit(lo);
            if (hv < 0 || lv < 0) {
                sodium_memzero(master_key_.data(), master_key_.size());
                throw std::runtime_error(
                    "PlaidTokenBroker: TF_MASTER_KEY contains non-hex characters.");
            }
            master_key_[i] = static_cast<std::byte>((hv << 4) | lv);
        }
    } else if (key_str.size() == 32) {
        // Raw bytes (unusual via env var but supported).
        std::memcpy(master_key_.data(), key_str.data(), 32);
    } else {
        throw std::runtime_error(
            "PlaidTokenBroker: TF_MASTER_KEY must be 64 hex characters or 32 raw bytes; "
            "got " + std::to_string(key_str.size()) + " characters.");
    }

    // Scrub key_str from stack / heap before deriving DEK.
    sodium_memzero(key_str.data(), key_str.size());

    dek_ = derive_dek(master_key_);
}

// ---------------------------------------------------------------------------
// Destructor — zero key material.
// ---------------------------------------------------------------------------
PlaidTokenBroker::~PlaidTokenBroker() {
    sodium_memzero(master_key_.data(), master_key_.size());
    // dek_ is a tf::crypto::EnvelopeKey, which zeroes itself in its destructor.
}

// ---------------------------------------------------------------------------
// store_token
// ---------------------------------------------------------------------------
void PlaidTokenBroker::store_token(const std::string& account_id,
                                    std::span<const std::byte> plaintext_token)
{
    // Build AAD from account_id bytes (AAD-binds ciphertext to this account).
    auto aad = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(account_id.data()),
        account_id.size());

    // Encrypt.
    std::vector<std::byte> blob = tf::crypto::encrypt(plaintext_token, aad, dek_);

    // Persist: UPDATE accounts SET encrypted_token=blob, is_plaid_linked=1 WHERE id=?
    auto stmt = db_.prepare(
        "UPDATE accounts SET encrypted_token=?, is_plaid_linked=1 WHERE id=?;");

    sqlite3_bind_blob(stmt.get(), 1,
                      blob.data(), static_cast<int>(blob.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2,
                      account_id.c_str(), static_cast<int>(account_id.size()),
                      SQLITE_STATIC);

    int rc = stmt.step();
    if (rc != SQLITE_DONE) {
        sodium_memzero(blob.data(), blob.size());
        throw std::runtime_error(
            "PlaidTokenBroker::store_token: UPDATE failed for account " + account_id);
    }

    // Scrub the blob before it goes out of scope (belt-and-suspenders; the
    // destructor of std::vector does NOT zero memory).
    sodium_memzero(blob.data(), blob.size());
}

void PlaidTokenBroker::store_token(const std::string& account_id,
                                    const std::string& plaintext_token)
{
    store_token(account_id,
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(plaintext_token.data()),
                    plaintext_token.size()));
}

// ---------------------------------------------------------------------------
// clear_token
// ---------------------------------------------------------------------------
void PlaidTokenBroker::clear_token(const std::string& account_id) {
    auto stmt = db_.prepare(
        "UPDATE accounts SET encrypted_token=NULL, is_plaid_linked=0 WHERE id=?;");
    sqlite3_bind_text(stmt.get(), 1,
                      account_id.c_str(), static_cast<int>(account_id.size()),
                      SQLITE_STATIC);
    // Ignore SQLITE_DONE vs rows-changed check — no-op if account missing.
    stmt.step();
}

// ---------------------------------------------------------------------------
// fetch_encrypted_blob (private)
// ---------------------------------------------------------------------------
std::optional<std::vector<std::byte>> PlaidTokenBroker::fetch_encrypted_blob(
    const std::string& account_id)
{
    auto stmt = db_.prepare(
        "SELECT encrypted_token FROM accounts WHERE id=?;");
    sqlite3_bind_text(stmt.get(), 1,
                      account_id.c_str(), static_cast<int>(account_id.size()),
                      SQLITE_STATIC);

    int rc = stmt.step();
    if (rc != SQLITE_ROW) {
        // No such account or no rows.
        return std::nullopt;
    }

    if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) {
        // Column is NULL — no token stored.
        return std::nullopt;
    }

    const void* blob_ptr = sqlite3_column_blob(stmt.get(), 0);
    int blob_len = sqlite3_column_bytes(stmt.get(), 0);

    if (!blob_ptr || blob_len <= 0) {
        return std::nullopt;
    }

    const std::byte* bytes = static_cast<const std::byte*>(blob_ptr);
    return std::vector<std::byte>(bytes, bytes + blob_len);
}

} // namespace tf::plaid
