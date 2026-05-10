#include "Totp.h"
#include "HmacSha1.h"

#include "../../src/services/crypto/ConstantTime.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::auth {

// ---------------------------------------------------------------------------
// generate_totp_secret
// ---------------------------------------------------------------------------
std::vector<std::byte> generate_totp_secret() {
    // 20 bytes is the standard TOTP secret length (RFC 4226 recommends
    // at least 128 bits = 16 bytes; 20 bytes = 160 bits is conventional).
    std::vector<std::byte> secret(20);
    randombytes_buf(secret.data(), secret.size());
    return secret;
}

// ---------------------------------------------------------------------------
// totp_secret_to_base32
// ---------------------------------------------------------------------------
// RFC 4648 base32 alphabet (uppercase).  No padding ('=') — most TOTP apps
// accept padding-free encoding and it's cleaner in provisioning URIs.
// ---------------------------------------------------------------------------
static const char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

std::string totp_secret_to_base32(std::span<const std::byte> secret) {
    std::string out;
    out.reserve((secret.size() * 8 + 4) / 5);

    int buf = 0;
    int bits = 0;
    for (auto b : secret) {
        buf = (buf << 8) | static_cast<uint8_t>(b);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out += kBase32Alphabet[(buf >> bits) & 0x1F];
        }
    }
    if (bits > 0) {
        out += kBase32Alphabet[(buf << (5 - bits)) & 0x1F];
    }
    return out;
}

// ---------------------------------------------------------------------------
// url_encode_component — percent-encode characters not safe in query values.
// ---------------------------------------------------------------------------
static std::string url_encode(std::string_view in) {
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        // Keep unreserved characters: ALPHA / DIGIT / "-" / "_" / "." / "~"
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// make_totp_provisioning_uri
// ---------------------------------------------------------------------------
std::string make_totp_provisioning_uri(std::string_view label,
                                       std::string_view issuer,
                                       std::span<const std::byte> secret) {
    std::string b32 = totp_secret_to_base32(secret);
    std::string enc_label  = url_encode(label);
    std::string enc_issuer = url_encode(issuer);
    std::string enc_b32    = url_encode(b32);

    return std::string("otpauth://totp/")
         + enc_issuer + ":" + enc_label
         + "?secret=" + enc_b32
         + "&issuer=" + enc_issuer
         + "&algorithm=SHA1&digits=6&period=30";
}

// ---------------------------------------------------------------------------
// compute_totp_code
// ---------------------------------------------------------------------------
// RFC 6238 / RFC 4226 algorithm:
//   1. counter = floor(unix_seconds / 30)  as big-endian uint64
//   2. HMAC-SHA1(secret, counter_bytes)
//   3. offset = hmac_result[19] & 0x0F
//   4. P = (hmac[offset] & 0x7F) << 24 | hmac[offset+1] << 16
//              | hmac[offset+2] << 8 | hmac[offset+3]
//   5. code = P % 10^6
// ---------------------------------------------------------------------------
int compute_totp_code(std::span<const std::byte> secret, int64_t unix_seconds) {
    // Step 1: compute counter as big-endian 8-byte value.
    uint64_t counter = static_cast<uint64_t>(unix_seconds) / 30;
    uint8_t counter_bytes[8];
    for (int i = 7; i >= 0; --i) {
        counter_bytes[i] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }

    // Step 2: HMAC-SHA1 via self-contained implementation (libsodium does not
    // expose crypto_auth_hmacsha1 in modern Homebrew builds; OpenSSL is already
    // linked for TLS but we use our header-only HmacSha1 to avoid OpenSSL API
    // coupling in the auth module).
    auto hmac_digest = tf::auth::hmacsha1::hmac(
        reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
        counter_bytes, sizeof(counter_bytes)
    );
    const uint8_t* hmac = hmac_digest.data();

    // Steps 3-5: dynamic truncation + modulo.
    uint8_t offset = hmac[19] & 0x0F;
    uint32_t p = ((hmac[offset]     & 0x7F) << 24)
               | ((hmac[offset + 1] & 0xFF) << 16)
               | ((hmac[offset + 2] & 0xFF) <<  8)
               |  (hmac[offset + 3] & 0xFF);
    return static_cast<int>(p % 1000000);
}

// ---------------------------------------------------------------------------
// verify_totp_code
// ---------------------------------------------------------------------------
// GUARDRAIL F-2: we must NOT short-circuit on first match.  We check every
// step in the window and accumulate the result.  Two int values are compared
// as equal by encoding them identically and using constant_time::equal on the
// raw bytes — this avoids branching on the comparison result.
// ---------------------------------------------------------------------------
bool verify_totp_code(std::span<const std::byte> secret,
                      int64_t unix_seconds,
                      int code,
                      int skew_window) {
    if (code < 0 || code > 999999) {
        return false;
    }

    // Encode the user-supplied code as a 4-byte big-endian int for CT compare.
    uint32_t code_u = static_cast<uint32_t>(code);
    uint8_t code_bytes[4] = {
        static_cast<uint8_t>((code_u >> 24) & 0xFF),
        static_cast<uint8_t>((code_u >> 16) & 0xFF),
        static_cast<uint8_t>((code_u >>  8) & 0xFF),
        static_cast<uint8_t>( code_u        & 0xFF)
    };

    int match = 0; // accumulate: set to 1 if any candidate matches
    for (int delta = -skew_window; delta <= skew_window; ++delta) {
        int64_t t = unix_seconds + static_cast<int64_t>(delta) * 30;
        int candidate = compute_totp_code(secret, t);

        uint32_t cand_u = static_cast<uint32_t>(candidate);
        uint8_t cand_bytes[4] = {
            static_cast<uint8_t>((cand_u >> 24) & 0xFF),
            static_cast<uint8_t>((cand_u >> 16) & 0xFF),
            static_cast<uint8_t>((cand_u >>  8) & 0xFF),
            static_cast<uint8_t>( cand_u        & 0xFF)
        };

        // sodium_memcmp returns 0 on equal.
        int eq = (sodium_memcmp(code_bytes, cand_bytes, 4) == 0) ? 1 : 0;
        match |= eq;
    }

    return match == 1;
}

} // namespace tf::auth
