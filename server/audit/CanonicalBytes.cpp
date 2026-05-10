#include "CanonicalBytes.h"

#include <sodium.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::audit {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Append a big-endian uint64 to `out`.
static void append_be64(std::vector<std::byte>& out, uint64_t v) {
    out.push_back(static_cast<std::byte>((v >> 56) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 48) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 40) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 32) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >>  8) & 0xFF));
    out.push_back(static_cast<std::byte>((v      ) & 0xFF));
}

// Append a big-endian int64 to `out` (bitcast to uint64).
static void append_be_int64(std::vector<std::byte>& out, int64_t v) {
    append_be64(out, static_cast<uint64_t>(v));
}

// Append a big-endian uint32 to `out`.
static void append_be32(std::vector<std::byte>& out, uint32_t v) {
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >>  8) & 0xFF));
    out.push_back(static_cast<std::byte>((v      ) & 0xFF));
}

// Append utf8 bytes followed by a NUL terminator.
static void append_nul_terminated(std::vector<std::byte>& out, const std::string& s) {
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    out.push_back(std::byte{0x00});
}

// ---------------------------------------------------------------------------
// compute_canonical_bytes
// ---------------------------------------------------------------------------

std::vector<std::byte> compute_canonical_bytes(const PreCommitEntry& e) {
    if (e.prev_hash.size() != 32) {
        throw std::invalid_argument(
            "compute_canonical_bytes: prev_hash must be exactly 32 bytes, got " +
            std::to_string(e.prev_hash.size()));
    }

    std::vector<std::byte> out;
    out.reserve(256);

    // seq: uint64 BE (plan uses int64 but seq is logically unsigned; cast safely)
    append_be64(out, static_cast<uint64_t>(e.seq));

    // tsUnixNanos: int64 BE = ts_ms * 1_000_000 exactly once (GUARDRAIL F-4)
    // NEVER add a sub-millisecond component here.
    const int64_t ts_unix_nanos = e.ts_ms * INT64_C(1'000'000);
    append_be_int64(out, ts_unix_nanos);

    // String fields, each NUL-terminated
    append_nul_terminated(out, e.actor_user_id);
    append_nul_terminated(out, e.actor_kind);
    append_nul_terminated(out, e.domain);
    append_nul_terminated(out, e.subject_id);
    append_nul_terminated(out, e.subject_kind);
    append_nul_terminated(out, e.action);
    append_nul_terminated(out, e.outcome);

    // detailsJson: len32be || bytes
    const uint32_t details_len = static_cast<uint32_t>(e.details_json.size());
    append_be32(out, details_len);
    for (char c : e.details_json) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }

    // prevHash: exactly 32 bytes
    for (std::byte b : e.prev_hash) {
        out.push_back(b);
    }

    return out;
}

// ---------------------------------------------------------------------------
// compute_entry_hash
// ---------------------------------------------------------------------------

std::vector<std::byte> compute_entry_hash(const std::vector<std::byte>& canonical) {
    std::vector<std::byte> hash(crypto_generichash_BYTES, std::byte{0}); // 32 bytes BLAKE2b-256

    static_assert(crypto_generichash_BYTES == 32,
        "Expected BLAKE2b output to be 32 bytes");

    int rc = crypto_generichash(
        reinterpret_cast<unsigned char*>(hash.data()),
        hash.size(),
        reinterpret_cast<const unsigned char*>(canonical.data()),
        canonical.size(),
        /*key=*/nullptr,
        /*keylen=*/0
    );
    if (rc != 0) {
        throw std::runtime_error("compute_entry_hash: crypto_generichash failed");
    }
    return hash;
}

} // namespace tf::audit
