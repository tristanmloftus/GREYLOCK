// EnvelopeEncryption.cpp — Implementation of envelope encrypt/decrypt.
//
// See EnvelopeEncryption.h for wire format, cipher choice, and guardrail notes.

#include "EnvelopeEncryption.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace tf::crypto {

namespace {

// Minimum blob size: version (1) + nonce (24) + tag (16) = 41 bytes.
// A zero-length plaintext produces a blob of exactly 41 bytes.
static constexpr std::size_t NONCE_BYTES =
    crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24

static constexpr std::size_t TAG_BYTES =
    crypto_aead_xchacha20poly1305_ietf_ABYTES; // 16

static constexpr std::size_t HEADER_BYTES = 1 + NONCE_BYTES; // 25

static constexpr std::size_t MIN_BLOB_BYTES = HEADER_BYTES + TAG_BYTES; // 41

// Ensure sodium is initialized. Throws on hard failure.
// Returns immediately (no-op) if already initialized.
void ensure_sodium_init() {
    int rc = sodium_init();
    if (rc == -1) {
        throw std::runtime_error(
            "tf::crypto: sodium_init() returned -1 — libsodium initialization failed. "
            "Check that the library is correctly installed and linked.");
    }
    // rc == 0: first init; rc == 1: already initialized. Both are fine.
}

} // namespace

// ---------------------------------------------------------------------------
// encrypt
// ---------------------------------------------------------------------------
std::vector<std::byte> encrypt(std::span<const std::byte> plaintext,
                                std::span<const std::byte> aad,
                                const EnvelopeKey& key) {
    ensure_sodium_init();

    // Allocate output: version(1) + nonce(24) + ciphertext(n + 16)
    const std::size_t ciphertext_len = plaintext.size() + TAG_BYTES;
    std::vector<std::byte> blob(HEADER_BYTES + ciphertext_len);

    // Write version byte.
    blob[0] = static_cast<std::byte>(ENVELOPE_VERSION);

    // Generate random nonce.
    unsigned char* nonce_ptr =
        reinterpret_cast<unsigned char*>(blob.data() + 1);
    randombytes_buf(nonce_ptr, NONCE_BYTES);

    // Encrypt + authenticate.
    unsigned char* ct_ptr =
        reinterpret_cast<unsigned char*>(blob.data() + HEADER_BYTES);
    unsigned long long actual_ct_len = 0;

    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        ct_ptr,
        &actual_ct_len,
        reinterpret_cast<const unsigned char*>(plaintext.data()),
        static_cast<unsigned long long>(plaintext.size()),
        aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(aad.data()),
        static_cast<unsigned long long>(aad.size()),
        nullptr,           // nsec — unused (always null for this construction)
        nonce_ptr,
        key.sodium_ptr()
    );

    if (rc != 0) {
        // libsodium's encrypt should never fail for valid inputs; treat as fatal.
        sodium_memzero(blob.data(), blob.size());
        throw std::runtime_error("tf::crypto::encrypt: libsodium encrypt returned non-zero");
    }

    assert(actual_ct_len == ciphertext_len);
    return blob;
}

// ---------------------------------------------------------------------------
// decrypt
// ---------------------------------------------------------------------------
std::optional<std::vector<std::byte>> decrypt(std::span<const std::byte> blob,
                                               std::span<const std::byte> aad,
                                               const EnvelopeKey& key) {
    ensure_sodium_init();

    // Validate minimum length.
    if (blob.size() < MIN_BLOB_BYTES) {
        return std::nullopt;
    }

    // Validate version byte.
    if (static_cast<std::uint8_t>(blob[0]) != ENVELOPE_VERSION) {
        return std::nullopt;
    }

    const unsigned char* nonce_ptr =
        reinterpret_cast<const unsigned char*>(blob.data() + 1);
    const unsigned char* ct_ptr =
        reinterpret_cast<const unsigned char*>(blob.data() + HEADER_BYTES);
    const unsigned long long ct_len =
        static_cast<unsigned long long>(blob.size() - HEADER_BYTES);

    // Plaintext length = ciphertext length - tag length.
    // ct_len >= TAG_BYTES because blob.size() >= MIN_BLOB_BYTES.
    const std::size_t plaintext_len =
        static_cast<std::size_t>(ct_len - TAG_BYTES);

    std::vector<std::byte> plaintext(plaintext_len);
    unsigned long long actual_pt_len = 0;

    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plaintext.data()),
        &actual_pt_len,
        nullptr,           // nsec — unused
        ct_ptr,
        ct_len,
        aad.empty() ? nullptr : reinterpret_cast<const unsigned char*>(aad.data()),
        static_cast<unsigned long long>(aad.size()),
        nonce_ptr,
        key.sodium_ptr()
    );

    if (rc != 0) {
        // Tag verification failed or corrupted ciphertext.
        // Scrub the partial plaintext buffer before returning nullopt.
        sodium_memzero(plaintext.data(), plaintext.size());
        return std::nullopt;
    }

    assert(actual_pt_len == plaintext_len);
    return plaintext;
}

} // namespace tf::crypto
