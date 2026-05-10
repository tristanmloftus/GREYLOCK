#include "Sanitizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace tf::audit {

// ---------------------------------------------------------------------------
// Constants (mirrored from lib/audit/sanitizer.ts)
// ---------------------------------------------------------------------------

static constexpr int MAX_DEPTH = 8;
static constexpr size_t MAX_TOTAL_BYTES = 64 * 1024; // 64 KiB

// Allowed keys — safe identifiers even when they substring-match the deny list.
// "tokenId" carves out from "token"; "key" fields not in allow-list are denied.
// Keys are stored in lowercase so lookup is case-insensitive (consistent with
// the case-insensitive deny-list policy).
static const std::unordered_set<std::string> ALLOWED_KEYS = {
    "userid",
    "sessionid",
    "accountid",
    "transactionid",
    "entityid",
    "tokenid",           // carve-out: id of the token row, never the token bytes
    "subjectid",
    "actoruserid",
    "domain",
    "outcome",
    "action",
    "kind",
    "reason",
    "version",
    "seq",
    "ts",
    "count",
    "added",
    "modified",
    "removed",
    "httpstatus",
    "errorcode",
    "transports",
};

// Deny-key substrings (case-insensitive).  A key matching any of these is
// rejected unless it appears in ALLOWED_KEYS.
// GUARDRAIL F-2: this list MUST be checked case-insensitively (via tolower).
static const std::vector<std::string> DENY_KEY_SUBSTRINGS = {
    "password",
    "passphrase",
    "secret",
    "token",             // covers access_token, refresh_token, link_token, …
    "cookie",
    "dek",
    "kek",
    "keksalt",
    "credentialpublickey",
    "signature",
    "pem",
    "key",               // intentionally last; carve-out via ALLOWED_KEYS
};

// ---------------------------------------------------------------------------
// Internal result type for recursive walk
// ---------------------------------------------------------------------------

struct WalkResult {
    bool ok{true};
    std::string reason;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string to_lower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

static bool is_key_allowed(const std::string& key) {
    // Lowercase once; use for both allowlist and deny-list lookups so
    // case-sensitivity is consistent across both checks (GUARDRAIL F-2).
    const std::string key_lower = to_lower(key);

    // Allowlist takes precedence.
    if (ALLOWED_KEYS.count(key_lower) > 0) {
        return true;
    }
    for (const auto& sub : DENY_KEY_SUBSTRINGS) {
        if (key_lower.find(sub) != std::string::npos) {
            return false;
        }
    }
    return true;
}

// Token-shape patterns (per plan §f and sanitizer.ts):
//   base64url-like: [A-Za-z0-9+/_-]{32,} with optional trailing =  (covers standard + url-safe base64)
//   hex-like:       [0-9a-fA-F]{32,}
// Per the task brief: /^[A-Za-z0-9+/_-]+={0,2}$/ or /^[0-9a-fA-F]{32,}$/
static bool is_base64url_like(const std::string& s) {
    // Must be at least 32 chars
    if (s.size() < 32) return false;
    // Optional trailing '=' (at most 2)
    size_t len = s.size();
    size_t padding = 0;
    while (padding < 2 && len > 0 && s[len - 1] == '=') {
        --len;
        ++padding;
    }
    // Body chars must be [A-Za-z0-9+/_-]
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '+' || c == '/' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool is_hex_like(const std::string& s) {
    if (s.size() < 32) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool looks_like_token(const std::string& s) {
    if (s.size() < 32) return false;
    // Check hex first (stricter subset of base64url chars)
    if (is_hex_like(s)) return true;
    if (is_base64url_like(s)) return true;
    return false;
}

// Forward declarations for mutual recursion.
static WalkResult walk_value(const nlohmann::json& value, int depth, const std::string& path);
static WalkResult walk_object(const nlohmann::json& obj, int depth, const std::string& path);
static WalkResult walk_array(const nlohmann::json& arr, int depth, const std::string& path);

static WalkResult walk_object(const nlohmann::json& obj, int depth, const std::string& path) {
    if (depth > MAX_DEPTH) {
        return {false, "depth limit at " + path};
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        if (!is_key_allowed(key)) {
            return {false, "deny key '" + key + "' at " + path};
        }
        WalkResult r = walk_value(it.value(), depth + 1, path + "." + key);
        if (!r.ok) return r;
    }
    return {true, {}};
}

static WalkResult walk_array(const nlohmann::json& arr, int depth, const std::string& path) {
    if (depth > MAX_DEPTH) {
        return {false, "depth limit at " + path};
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        WalkResult r = walk_value(arr[i], depth + 1, path + "[" + std::to_string(i) + "]");
        if (!r.ok) return r;
    }
    return {true, {}};
}

static WalkResult walk_value(const nlohmann::json& value, int depth, const std::string& path) {
    if (value.is_null()) {
        return {true, {}};
    }
    if (value.is_string()) {
        const std::string& s = value.get<std::string>();
        if (looks_like_token(s)) {
            return {false, "token-shape value at " + path};
        }
        return {true, {}};
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return {true, {}};
    }
    if (value.is_number_float()) {
        double d = value.get<double>();
        if (!std::isfinite(d)) {
            return {false, "non-finite number at " + path};
        }
        return {true, {}};
    }
    if (value.is_boolean()) {
        return {true, {}};
    }
    if (value.is_array()) {
        return walk_array(value, depth, path);
    }
    if (value.is_object()) {
        return walk_object(value, depth, path);
    }
    // Anything else (binary, discarded) is rejected.
    return {false, "unsupported value kind at " + path};
}

// ---------------------------------------------------------------------------
// sanitize (public)
// ---------------------------------------------------------------------------

SanitizerResult sanitize(const nlohmann::json& details) noexcept {
    try {
        if (!details.is_object()) {
            return {SanitizerStatus::Rejected, "details must be a JSON object"};
        }

        // Recursive walk starting at depth 0.
        WalkResult r = walk_object(details, 0, "details");
        if (!r.ok) {
            return {SanitizerStatus::Rejected, r.reason};
        }

        // Total serialized size cap (64 KiB).
        std::string serialized = details.dump();
        if (serialized.size() > MAX_TOTAL_BYTES) {
            return {SanitizerStatus::Rejected, "payload exceeds 64 KiB"};
        }

        return {SanitizerStatus::Accepted, {}};
    } catch (...) {
        // Defensive: never propagate exceptions.
        return {SanitizerStatus::Rejected, "sanitizer internal error"};
    }
}

} // namespace tf::audit
