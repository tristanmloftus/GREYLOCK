#pragma once

// HmacSha1.h — self-contained HMAC-SHA1 for RFC 6238 TOTP.
//
// libsodium's Homebrew build does not expose crypto_auth_hmacsha1 (SHA-1 was
// deprecated and removed from libsodium's public API).  OpenSSL is already
// linked for TLS but we avoid its HMAC API to keep the auth module portable.
//
// This implements RFC 2104 HMAC over a minimal RFC 3174 SHA-1.
// SHA-1 is used ONLY because RFC 6238 mandates it for TOTP compatibility
// with authenticator apps (Google Authenticator, Aegis, etc.).
//
// These are NOT used for any security-sensitive purpose beyond TOTP —
// the Argon2id/BLAKE2b path handles the security-critical operations.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace tf::auth::hmacsha1 {

static constexpr size_t kDigestLen = 20; // SHA-1 output bytes
static constexpr size_t kBlockLen  = 64; // SHA-1 block size bytes

// SHA-1 internal helpers (RFC 3174 §6.1).
namespace detail {

inline uint32_t rot32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

struct Sha1State {
    uint32_t h[5]{
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };
    uint64_t len_bits{0};
    uint8_t  buf[64]{};
    uint8_t  buf_used{0};

    void process_block(const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(b[i * 4])     << 24)
                 | (static_cast<uint32_t>(b[i * 4 + 1]) << 16)
                 | (static_cast<uint32_t>(b[i * 4 + 2]) <<  8)
                 |  static_cast<uint32_t>(b[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rot32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }
        uint32_t a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b2 & c) | (~b2 & d);       k = 0x5A827999u; }
            else if (i < 40) { f = b2 ^ c ^ d;                  k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b2 & c) | (b2 & d) | (c & d); k = 0x8F1BBCDCu; }
            else              { f = b2 ^ c ^ d;                  k = 0xCA62C1D6u; }
            uint32_t tmp = rot32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rot32(b2, 30); b2 = a; a = tmp;
        }
        h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        len_bits += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            size_t copy = std::min(len, static_cast<size_t>(64 - buf_used));
            std::memcpy(buf + buf_used, data, copy);
            buf_used += static_cast<uint8_t>(copy);
            data += copy;
            len  -= copy;
            if (buf_used == 64) {
                process_block(buf);
                buf_used = 0;
            }
        }
    }

    std::array<uint8_t, 20> finalize() {
        buf[buf_used++] = 0x80;
        if (buf_used > 56) {
            while (buf_used < 64) buf[buf_used++] = 0;
            process_block(buf);
            buf_used = 0;
        }
        while (buf_used < 56) buf[buf_used++] = 0;
        for (int i = 0; i < 8; ++i) {
            buf[56 + i] = static_cast<uint8_t>(
                (len_bits >> (56 - i * 8)) & 0xFF);
        }
        process_block(buf);

        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >>  8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
        return out;
    }
};

} // namespace detail

// Compute HMAC-SHA1(key, message) → 20-byte digest.
// RFC 2104: H((key ⊕ opad) || H((key ⊕ ipad) || message))
inline std::array<uint8_t, kDigestLen> hmac(
    const uint8_t* key,   size_t key_len,
    const uint8_t* msg,   size_t msg_len)
{
    uint8_t k[kBlockLen]{};
    if (key_len > kBlockLen) {
        // Hash the key if it's longer than one block.
        detail::Sha1State hs;
        hs.update(key, key_len);
        auto digest = hs.finalize();
        std::memcpy(k, digest.data(), digest.size());
    } else {
        std::memcpy(k, key, key_len);
    }

    uint8_t ipad[kBlockLen], opad[kBlockLen];
    for (size_t i = 0; i < kBlockLen; ++i) {
        ipad[i] = k[i] ^ 0x36u;
        opad[i] = k[i] ^ 0x5Cu;
    }

    // Inner hash: H(ipad || message)
    detail::Sha1State inner;
    inner.update(ipad, kBlockLen);
    inner.update(msg, msg_len);
    auto inner_digest = inner.finalize();

    // Outer hash: H(opad || inner_digest)
    detail::Sha1State outer;
    outer.update(opad, kBlockLen);
    outer.update(inner_digest.data(), inner_digest.size());
    return outer.finalize();
}

} // namespace tf::auth::hmacsha1
