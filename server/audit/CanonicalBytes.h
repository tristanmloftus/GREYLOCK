#pragma once

// CanonicalBytes.h — canonical byte layout for BLAKE2b-chained audit entries.
//
// GUARDRAIL F-4: tsUnixNanos = ts_ms * 1_000_000 exactly once.  No second
// timestamp term ever.  The canonical-bytes test (test_audit_chain_canonical_bytes_no_double_count)
// is the explicit regression gate for this invariant.
//
// Canonical layout (per plan §f):
//   seq         : uint64 BE    (8 bytes)
//   tsUnixNanos : int64  BE    (8 bytes)  = ts_ms * 1_000_000
//   actorUserId : utf8 || 0x00
//   actorKind   : utf8 || 0x00
//   domain      : utf8 || 0x00
//   subjectId   : utf8 || 0x00
//   subjectKind : utf8 || 0x00
//   action      : utf8 || 0x00
//   outcome     : utf8 || 0x00
//   detailsJson : len32be(bytes) || bytes
//   prevHash    : 32 bytes
//
// entry_hash = BLAKE2b-256(canonical bytes above)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tf::audit {

// All fields needed to produce canonical bytes for a pending entry.
// seq and prevHash are supplied by the writer at chain-append time.
struct PreCommitEntry {
    int64_t     seq{0};
    int64_t     ts_ms{0};          // ms since epoch; multiplied by 1e6 inside compute_canonical_bytes
    std::string actor_user_id;
    std::string actor_kind;
    std::string domain;
    std::string subject_id;
    std::string subject_kind;
    std::string action;
    std::string outcome;
    std::string details_json;      // serialized compact JSON
    std::vector<std::byte> prev_hash; // exactly 32 bytes
};

// Returns the canonical byte sequence as defined in plan §f.
// Throws std::invalid_argument if prev_hash.size() != 32.
std::vector<std::byte> compute_canonical_bytes(const PreCommitEntry& e);

// Returns BLAKE2b-256 (32 bytes) of the given byte sequence.
// Uses libsodium crypto_generichash.
std::vector<std::byte> compute_entry_hash(const std::vector<std::byte>& canonical);

} // namespace tf::audit
