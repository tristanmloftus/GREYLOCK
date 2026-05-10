#pragma once

// Sanitizer.h — audit-log details sanitizer (C++ port of lib/audit/sanitizer.ts).
//
// Enforces the closed-by-default deny list for audit event details.
// Rejection = whole append rejected; no strip-and-keep behavior.
//
// GUARDRAIL F-2: the deny list MUST actually reject the keys it claims to.
// Every deny-key substring check is case-insensitive.  The allowed-keys set
// carves out safe identifiers that substring-match the deny list
// (e.g. "tokenId" matches "token" but is explicitly allowed).
//
// Behavior:
//   - Pure function.  No I/O.  No exceptions — returns SanitizerResult.
//   - Recursive walk of the JSON object tree, capped at depth 8.
//   - Total serialized size capped at 64 KiB.
//   - Deny-list keys rejected; token-shape values (long base64/hex) rejected.
//   - Returns SanitizerResult::Accepted or SanitizerResult::Rejected.

#include <nlohmann/json.hpp>
#include <string>

namespace tf::audit {

enum class SanitizerStatus { Accepted, Rejected };

struct SanitizerResult {
    SanitizerStatus status;
    std::string reason; // populated on Rejected only
};

// Sanitize `details` (must be a JSON object).
// Returns Accepted if the payload passes all checks.
// Returns Rejected with a non-sensitive reason string otherwise.
// Never throws.
SanitizerResult sanitize(const nlohmann::json& details) noexcept;

} // namespace tf::audit
