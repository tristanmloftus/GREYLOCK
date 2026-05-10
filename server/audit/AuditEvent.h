#pragma once

// AuditEvent.h — canonical audit event structure (Phase 3).
//
// Phase 4 will wire this into a BLAKE2b-chained writer.  For now,
// StubAuditLog logs to stderr.
//
// GUARDRAIL F-4: callers must pass ts_ms as ms_since_epoch only; the
// IAuditLog implementation stores ts_unix_nanos = ts_ms * 1_000_000.
// Never pass a sub-millisecond second field.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace tf::audit {

struct AuditEvent {
    // Milliseconds since Unix epoch.  Written as-is; the chain writer
    // will compute ts_unix_nanos = ts_ms * 1'000'000.
    int64_t ts_ms{0};

    // Who performed the action.  Empty string = system/anonymous.
    std::string actor_user_id;
    // "user" | "system" | "sync_worker"
    std::string actor_kind;

    // Entity scope (entity UUID or "" if not entity-scoped).
    std::string domain;

    // Subject of the action (e.g. user_id, session_id, token_hash_hex).
    std::string subject_id;
    // "user" | "session" | "enrollment_token" | ...
    std::string subject_kind;

    // Verb describing what happened (see action name constants in AuthHandlers).
    std::string action;

    // "success" | "failure" | "rate_limited"
    std::string outcome;

    // Additional structured context.  MUST NOT contain passphrase, totp_code,
    // or raw session token.
    nlohmann::json details{nlohmann::json::object()};
};

} // namespace tf::audit
