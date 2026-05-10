#pragma once

// AuthHandlers.h — registers the four /auth/* routes (Phase 3).
//
// Routes:
//   POST /auth/enroll   — consume enrollment token, create user, return TOTP URI
//   POST /auth/login    — verify passphrase + TOTP, mint session
//   POST /auth/logout   — revoke session
//   GET  /auth/whoami   — validate-and-touch session, return user info
//
// GUARDRAIL F-3: every handler wraps its body in try/catch; the server
// must not crash on malformed JSON or unexpected DB errors.
//
// GUARDRAIL F-5: rate-limit bucket key is ("auth_login", email).
// X-Forwarded-For is never used for rate-limiting decisions.

// Forward-declare to avoid pulling httplib.h into this header.
namespace httplib { class SSLServer; }

#include "../../server/db/Database.h"
#include "../../server/audit/IAuditLog.h"

namespace tf::auth {

// Register all four /auth/* routes on the given SSLServer.
// Called once from server/main.cpp after migrations have been applied.
//
// db must outlive the server (routes hold a reference to it).
// audit_log must outlive the server.
void register_auth_handlers(httplib::SSLServer& server,
                              Database& db,
                              tf::audit::IAuditLog& audit_log);

} // namespace tf::auth
