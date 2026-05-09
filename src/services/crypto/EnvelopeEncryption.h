#pragma once

// EnvelopeEncryption.h — Authenticated envelope encryption for at-rest data.
//
// CIPHER CHOICE: crypto_aead_xchacha20poly1305_ietf_encrypt / _decrypt
//   (XChaCha20-Poly1305 IETF variant, libsodium).
//   Rationale: the plan (§e) calls for AEAD with an `aad` parameter. The
//   simpler `crypto_secretbox_xchacha20poly1305_easy` does not accept AAD;
//   the AEAD variant does. Same XChaCha20-Poly1305 primitive, same 192-bit
//   nonce, same 16-byte Poly1305 tag — just with AAD threading.
//
// WIRE FORMAT on disk (version 0x01):
//   version  (1 byte)  — currently 0x01
//   nonce   (24 bytes) — random, per-call, via randombytes_buf
//   ciphertext         — length = plaintext_len + ABYTES (16 bytes MAC/tag
//                        appended by the IETF variant in combined mode)
//
//   Total overhead per blob: 1 + 24 + 16 = 41 bytes.
//   Note: the IETF combined-mode function appends the tag AT THE END of the
//   ciphertext buffer, so the layout is:
//     [ version | nonce | encrypted_plaintext | poly1305_tag ]
//   This matches plan §e's "ciphertext || tag (16 bytes)" description.
//
// AAD: The aad parameter is forwarded to the AEAD primitive. v0.2 callers
//   may pass an empty span (no additional data). Future callers can bind a
//   context string (e.g. account_id || row_class) to prevent blob-swapping.
//
// GUARDRAIL F-1: EnvelopeKey is opaque. This API does not derive keys from
//   public identifiers. Key sourcing is the caller's responsibility.
// GUARDRAIL F-2: Tag verification uses libsodium's constant-time internal
//   logic; we do not compare tags ourselves.
// GUARDRAIL F-3: encrypt() and decrypt() call sodium_init() at entry and
//   throw std::runtime_error if it returns -1 (failure). A return of 0
//   (first init) or 1 (already initialized) both proceed normally. This
//   surfaces libsodium initialization failures explicitly rather than
//   silently continuing with undefined behavior.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <sodium.h>

namespace tf::crypto {

// ---------------------------------------------------------------------------
// EnvelopeKey
//
// 32-byte key for XChaCha20-Poly1305. Zeroed in destructor.
// Non-copyable. Movable.
// ---------------------------------------------------------------------------
struct EnvelopeKey {
    static constexpr std::size_t KEY_BYTES =
        crypto_aead_xchacha20poly1305_ietf_KEYBYTES; // 32

    std::array<std::byte, KEY_BYTES> bytes{};

    EnvelopeKey() = default;

    // Construct from a raw 32-byte span (e.g. from a KDF output).
    explicit EnvelopeKey(std::span<const std::byte, KEY_BYTES> src) {
        std::copy(src.begin(), src.end(), bytes.begin());
    }

    // Non-copyable.
    EnvelopeKey(const EnvelopeKey&)            = delete;
    EnvelopeKey& operator=(const EnvelopeKey&) = delete;

    // Movable.
    EnvelopeKey(EnvelopeKey&& other) noexcept : bytes(other.bytes) {
        sodium_memzero(other.bytes.data(), other.bytes.size());
    }
    EnvelopeKey& operator=(EnvelopeKey&& other) noexcept {
        if (this != &other) {
            sodium_memzero(bytes.data(), bytes.size());
            bytes = other.bytes;
            sodium_memzero(other.bytes.data(), other.bytes.size());
        }
        return *this;
    }

    ~EnvelopeKey() {
        sodium_memzero(bytes.data(), bytes.size());
    }

    // Convenience: view as unsigned char* for libsodium APIs.
    const unsigned char* sodium_ptr() const noexcept {
        return reinterpret_cast<const unsigned char*>(bytes.data());
    }
};

// ---------------------------------------------------------------------------
// Version byte
// ---------------------------------------------------------------------------
static constexpr std::uint8_t ENVELOPE_VERSION = 0x01;

// ---------------------------------------------------------------------------
// encrypt()
//
// Returns: version || nonce || ciphertext+tag  (version=0x01, nonce=24 bytes,
//          tag=16 bytes appended inside ciphertext by libsodium).
//
// Throws std::runtime_error if sodium_init() fails or if libsodium returns
// an unexpected error (should never happen for encrypt).
// ---------------------------------------------------------------------------
std::vector<std::byte> encrypt(std::span<const std::byte> plaintext,
                                std::span<const std::byte> aad,
                                const EnvelopeKey& key);

// ---------------------------------------------------------------------------
// decrypt()
//
// Returns std::nullopt on:
//   - blob too short to hold version + nonce + tag
//   - version byte != 0x01
//   - AEAD tag verification failure (including any tampering)
//   - sodium_init() failure
//
// NEVER returns garbage plaintext. The plaintext buffer is cleared before
// returning nullopt on tag-verify failure.
// ---------------------------------------------------------------------------
std::optional<std::vector<std::byte>> decrypt(std::span<const std::byte> blob,
                                               std::span<const std::byte> aad,
                                               const EnvelopeKey& key);

} // namespace tf::crypto
