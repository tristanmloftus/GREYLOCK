// AuthHandlers.cpp — /auth/* route implementations (Phase 3).
//
// GUARDRAILS (all enforced here):
//   F-1: session tokens come from randombytes_buf only.
//   F-2: passphrase verify and TOTP verify use constant-time primitives.
//   F-3: every handler wrapped in try/catch; never crashes the server.
//   F-4: audit timestamps passed as ms_since_epoch exactly once.
//   F-5: rate-limit key is ("auth_login", email); XFF never used for security.
//
// ABSOLUTELY NO passphrase, totp_code, or session token in any log line.

#include "httplib.h"

#include "AuthHandlers.h"
#include "AuthRateLimitInternal.h"
#include "EnrollmentToken.h"
#include "PassphraseHash.h"
#include "Session.h"
#include "Totp.h"

#include "../../server/audit/AuditEvent.h"
#include "../../server/audit/IAuditLog.h"
#include "../../server/db/Database.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace tf::auth {

// ---------------------------------------------------------------------------
// Timing-equalization dummy hash (C-1)
//
// Computed ONCE at first use (lazy static).  The passphrase value is not a
// real credential and is not secret — it is intentionally meaningless.  Its
// sole purpose is to produce a valid Argon2id hash string so that
// verify_passphrase() can perform the same ~500-900 ms of crypto work on the
// user-not-found path, preventing timing-based user-existence enumeration.
//
// Do NOT log this passphrase; it is not a secret but there is no reason to
// surface it in logs either.
// ---------------------------------------------------------------------------
static const std::vector<std::byte>& dummy_hash() {
    static const std::vector<std::byte> kDummy =
        hash_passphrase("tf-dummy-init-passphrase-do-not-use-9c4f");
    return kDummy;
}

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------

// Returns current milliseconds since Unix epoch (F-4: single source of truth).
static int64_t now_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Returns current Unix seconds.
static int64_t now_unix() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// UUID-like ID generator (using sodium random bytes hex-encoded).
// Used to generate user IDs.
// ---------------------------------------------------------------------------
static std::string generate_id() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

// ---------------------------------------------------------------------------
// Rate-limit state
// F-5: keyed on ("auth_login", email) — never on XFF or IP.
// Limit: 5 attempts per 15-minute window.
// In-process map; resets on server restart (acceptable for v0.2).
//
// DoS hardening (Phase 4.D deferred): the map is bounded by kMaxRateBuckets.
// If saturated, opportunistically sweep expired entries; if still full,
// fail closed (return rate-limited) so the saturation itself becomes a
// signal rather than an OOM vector.
//
// Implementation lives in the tf::auth::internal namespace (declared in
// AuthRateLimitInternal.h) so the unit test can reach it without spinning
// up an HTTP server.  Handlers below call the inline file-scope wrapper.
// ---------------------------------------------------------------------------

namespace {

struct RateBucket {
    int     count{0};
    int64_t window_start_unix{0};
};

std::mutex g_rate_mutex;
std::unordered_map<std::string, RateBucket> g_rate_buckets;

} // anonymous namespace

}  // close namespace tf::auth (re-opened below)

namespace tf::auth::internal {

// Returns true if this request should be rate-limited (bucket tripped).
// If not limited, increments the counter.
// If the window has expired, resets the bucket first.
//
// Bounded-growth contract: if g_rate_buckets is at kMaxRateBuckets and the
// requested bucket is new, first sweep expired entries; if the map is still
// at the cap after the sweep, return true (deny) WITHOUT inserting and emit
// a saturation warning to stderr.
bool rate_limit_check(const std::string& bucket_key, int64_t t_unix) {
    std::lock_guard<std::mutex> lock(g_rate_mutex);

    // If the key is not already present and the map is at the cap, try to
    // make room by evicting expired buckets before inserting.
    if (g_rate_buckets.find(bucket_key) == g_rate_buckets.end() &&
        g_rate_buckets.size() >= kMaxRateBuckets) {
        for (auto it = g_rate_buckets.begin(); it != g_rate_buckets.end(); ) {
            if (t_unix - it->second.window_start_unix >= kRateLimitWindowSecs) {
                it = g_rate_buckets.erase(it);
            } else {
                ++it;
            }
        }
        if (g_rate_buckets.size() >= kMaxRateBuckets) {
            // Fail closed: refuse to allocate a new bucket.  Saturation is
            // itself a signal worth surfacing — log once per denial.
            std::cerr << "AuthHandlers: rate-limit map saturated, denying "
                      << bucket_key << std::endl;
            return true;
        }
    }

    auto& bucket = g_rate_buckets[bucket_key];

    if (t_unix - bucket.window_start_unix >= kRateLimitWindowSecs) {
        // Window expired — reset.
        bucket.count = 0;
        bucket.window_start_unix = t_unix;
    }

    if (bucket.count >= kRateLimitMax) {
        return true; // tripped
    }

    ++bucket.count;
    return false;
}

void rate_limit_reset_for_tests() {
    std::lock_guard<std::mutex> lock(g_rate_mutex);
    g_rate_buckets.clear();
}

std::size_t rate_limit_bucket_count_for_tests() {
    std::lock_guard<std::mutex> lock(g_rate_mutex);
    return g_rate_buckets.size();
}

} // namespace tf::auth::internal

namespace tf::auth {

// Translation-unit-local alias keeps the existing handler call sites
// (rate_limit_check(bucket_key, t)) unchanged.
static inline bool rate_limit_check(const std::string& bucket_key, int64_t t_unix) {
    return ::tf::auth::internal::rate_limit_check(bucket_key, t_unix);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

static void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Extract Bearer token from Authorization header.
static std::optional<std::string> extract_bearer(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return std::nullopt;
    const std::string& hdr = it->second;
    if (hdr.size() < 8 || hdr.substr(0, 7) != "Bearer ") return std::nullopt;
    return hdr.substr(7);
}

// ---------------------------------------------------------------------------
// Audit helper
// ---------------------------------------------------------------------------

static void emit_audit(tf::audit::IAuditLog& log,
                        int64_t ts_ms,
                        const std::string& actor_user_id,
                        const std::string& actor_kind,
                        const std::string& domain,
                        const std::string& subject_id,
                        const std::string& subject_kind,
                        const std::string& action,
                        const std::string& outcome,
                        const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = ts_ms; // F-4: passed once, exactly as ms_since_epoch
    evt.actor_user_id = actor_user_id;
    evt.actor_kind    = actor_kind;
    evt.domain        = domain;
    evt.subject_id    = subject_id;
    evt.subject_kind  = subject_kind;
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;
    log.record(evt);
}

// ---------------------------------------------------------------------------
// POST /auth/enroll
// ---------------------------------------------------------------------------
// Body: {token, email, passphrase, totp_secret (optional base32 string)}
// Response: {user_id, totp_provisioning_uri}
// ---------------------------------------------------------------------------
static void handle_enroll(const httplib::Request& req,
                           httplib::Response& res,
                           Database& db,
                           tf::audit::IAuditLog& audit_log)
{
    int64_t ts = now_ms();
    int64_t t  = now_unix();

    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        send_json(res, 400, {{"error", "invalid_json"}});
        return;
    }

    std::string token_str, email_str, passphrase_str;
    try {
        token_str      = body.at("token").get<std::string>();
        email_str      = body.at("email").get<std::string>();
        passphrase_str = body.at("passphrase").get<std::string>();
    } catch (...) {
        send_json(res, 400, {{"error", "missing_fields"},
                              {"message", "token, email, passphrase required"}});
        return;
    }

    // H-1 + H-2: Token validation, email-match check, user creation, and token
    // consumption all happen inside ONE BEGIN IMMEDIATE transaction.
    //
    // Email-mismatch is deliberately indistinguishable from invalid-token in
    // both the HTTP response and the audit log action/outcome, so that callers
    // cannot enumerate valid (token, email) pairs by trying different emails.
    db.exec("BEGIN IMMEDIATE;");
    try {
        // Step 1: Peek (don't consume yet) to validate the token and get its email.
        auto peeked = peek_enrollment_token(db, token_str, t);
        if (!peeked.has_value()) {
            db.exec("ROLLBACK;");
            emit_audit(audit_log, ts, "", "system", "", "", "enrollment_token",
                       "enrollment_token_rejected", "failure");
            send_json(res, 400, {{"error", "invalid_enrollment_token"}});
            return;
        }

        // Step 2: Email-match check.  If MISMATCH, treat exactly as invalid
        //         token — do not consume the token, do not reveal its validity.
        if (peeked->email != email_str) {
            db.exec("ROLLBACK;");
            emit_audit(audit_log, ts, "", "system", "", "", "enrollment_token",
                       "enrollment_token_rejected", "failure");
            send_json(res, 400, {{"error", "invalid_enrollment_token"}});
            return;
        }

        // Step 3: Hash the passphrase (slow; ~0.7 s).  Done inside the
        //         transaction so we hold the write lock while hashing — the
        //         BEGIN IMMEDIATE already grabbed the write lock.
        std::vector<std::byte> pass_hash;
        try {
            pass_hash = hash_passphrase(passphrase_str);
            sodium_memzero(passphrase_str.data(), passphrase_str.size());
        } catch (const std::exception& ex) {
            db.exec("ROLLBACK;");
            send_json(res, 500, {{"error", "internal"}, {"message", ex.what()}});
            return;
        }

        // Step 4: Generate TOTP secret and user ID.
        std::vector<std::byte> totp_secret = generate_totp_secret();
        std::string user_id = generate_id();

        // Step 5: INSERT user.
        {
            auto stmt = db.prepare(
                "INSERT INTO users (id, email, created_at_unix, passphrase_hash, totp_secret) "
                "VALUES (?, ?, ?, ?, ?);"
            );
            sqlite3_bind_text(stmt.get(), 1,
                user_id.data(), static_cast<int>(user_id.size()), SQLITE_STATIC);
            sqlite3_bind_text(stmt.get(), 2,
                email_str.data(), static_cast<int>(email_str.size()), SQLITE_STATIC);
            sqlite3_bind_int64(stmt.get(), 3, t);
            sqlite3_bind_blob(stmt.get(), 4,
                pass_hash.data(), static_cast<int>(pass_hash.size()), SQLITE_STATIC);
            sqlite3_bind_blob(stmt.get(), 5,
                totp_secret.data(), static_cast<int>(totp_secret.size()), SQLITE_STATIC);

            int rc = stmt.step();
            if (rc != SQLITE_DONE) {
                db.exec("ROLLBACK;");
                // Likely email uniqueness violation.
                send_json(res, 409, {{"error", "email_already_registered"}});
                return;
            }
        }

        // Step 6: Mark token consumed (now that user creation succeeded).
        if (!mark_enrollment_token_consumed(db, peeked->token_hash, t)) {
            db.exec("ROLLBACK;");
            send_json(res, 500, {{"error", "internal"},
                                  {"message", "token consume failed"}});
            return;
        }

        // Step 7: Commit everything atomically.
        db.exec("COMMIT;");

        std::string uri = make_totp_provisioning_uri(email_str, "TerminalFinance",
                                                      totp_secret);

        emit_audit(audit_log, ts, user_id, "system", "", user_id, "user",
                   "enrollment_token_consumed", "success");

        send_json(res, 200, {
            {"user_id", user_id},
            {"totp_provisioning_uri", uri}
        });

    } catch (...) {
        char* errmsg = nullptr;
        sqlite3_exec(db.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
        sqlite3_free(errmsg);
        send_json(res, 500, {{"error", "internal"}});
    }
}

// ---------------------------------------------------------------------------
// POST /auth/login
// ---------------------------------------------------------------------------
// Body: {email, passphrase, totp_code (integer)}
// Response: {session_token, user_id, expires_at_unix}
// ---------------------------------------------------------------------------
static void handle_login(const httplib::Request& req,
                          httplib::Response& res,
                          Database& db,
                          tf::audit::IAuditLog& audit_log)
{
    int64_t ts = now_ms();
    int64_t t  = now_unix();

    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        send_json(res, 400, {{"error", "invalid_json"}});
        return;
    }

    std::string email_str, passphrase_str;
    int totp_code = -1;
    try {
        email_str      = body.at("email").get<std::string>();
        passphrase_str = body.at("passphrase").get<std::string>();
        totp_code      = body.at("totp_code").get<int>();
    } catch (...) {
        send_json(res, 400, {{"error", "missing_fields"},
                              {"message", "email, passphrase, totp_code required"}});
        return;
    }

    // F-5: rate-limit key is ("auth_login", email). NEVER XFF.
    std::string bucket_key = "auth_login:" + email_str;
    if (rate_limit_check(bucket_key, t)) {
        emit_audit(audit_log, ts, "", "system", "", email_str, "user",
                   "auth_rate_limit_tripped", "rate_limited",
                   {{"email", email_str}});
        send_json(res, 429, {{"error", "rate_limited"},
                              {"message", "Too many login attempts. Try again later."}});
        return;
    }

    // Look up user by email.
    std::string user_id_db;
    std::vector<std::byte> pass_hash_db;
    std::vector<std::byte> totp_secret_db;

    {
        auto stmt = db.prepare(
            "SELECT id, passphrase_hash, totp_secret FROM users WHERE email = ?;"
        );
        sqlite3_bind_text(stmt.get(), 1,
            email_str.data(), static_cast<int>(email_str.size()), SQLITE_STATIC);

        int rc = stmt.step();
        if (rc != SQLITE_ROW) {
            // C-1: Timing equalization — perform the SAME Argon2id verify work
            // regardless of whether the email exists.  This prevents a
            // timing-based side channel that would reveal user existence.
            // dummy_hash() is pre-computed once at startup; result is discarded.
            verify_passphrase(passphrase_str, dummy_hash());
            sodium_memzero(passphrase_str.data(), passphrase_str.size());
            // Use the SAME action name and response shape as wrong-passphrase
            // failures so neither audit logs nor response timing reveal which
            // arm was taken.
            emit_audit(audit_log, ts, "", "system", "", "", "user",
                       "passphrase_authentication_failure", "failure");
            send_json(res, 401, {{"error", "auth_failed"}});
            return;
        }

        const char* uid = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 0));
        if (uid) user_id_db = uid;

        const void* hash_data = sqlite3_column_blob(stmt.get(), 1);
        int hash_len          = sqlite3_column_bytes(stmt.get(), 1);
        if (hash_data && hash_len > 0) {
            pass_hash_db.assign(
                static_cast<const std::byte*>(hash_data),
                static_cast<const std::byte*>(hash_data) + hash_len);
        }

        const void* totp_data = sqlite3_column_blob(stmt.get(), 2);
        int totp_len          = sqlite3_column_bytes(stmt.get(), 2);
        if (totp_data && totp_len > 0) {
            totp_secret_db.assign(
                static_cast<const std::byte*>(totp_data),
                static_cast<const std::byte*>(totp_data) + totp_len);
        }
    }

    // Verify passphrase (constant-time via libsodium).
    bool pass_ok = verify_passphrase(passphrase_str, pass_hash_db);
    sodium_memzero(passphrase_str.data(), passphrase_str.size());

    if (!pass_ok) {
        emit_audit(audit_log, ts, user_id_db, "user", "", user_id_db, "user",
                   "passphrase_authentication_failure", "failure",
                   {{"reason", "wrong_passphrase"}});
        send_json(res, 401, {{"error", "auth_failed"}});
        return;
    }

    // Verify TOTP (constant-time window check, ±1 step).
    bool totp_ok = verify_totp_code(totp_secret_db, t, totp_code, 1);
    if (!totp_ok) {
        emit_audit(audit_log, ts, user_id_db, "user", "", user_id_db, "user",
                   "passphrase_authentication_failure", "failure",
                   {{"reason", "wrong_totp"}});
        send_json(res, 401, {{"error", "auth_failed"}});
        return;
    }

    // Mint session.
    MintedSession session;
    try {
        session = mint_session(db, user_id_db, t);
    } catch (const std::exception& ex) {
        send_json(res, 500, {{"error", "internal"}, {"message", ex.what()}});
        return;
    }

    int64_t expires_at = t + kSessionAbsoluteTimeoutSeconds;

    emit_audit(audit_log, ts, user_id_db, "user", "", session.token_hash.empty()
        ? ""
        : [&]() {
            char hx[65];
            sodium_bin2hex(hx, sizeof(hx),
                reinterpret_cast<const unsigned char*>(session.token_hash.data()),
                session.token_hash.size());
            return std::string(hx);
          }(),
        "session",
        "passphrase_authentication_success", "success",
        {{"email", email_str}});

    emit_audit(audit_log, ts, user_id_db, "user", "", user_id_db, "user",
               "session_created", "success", json::object());

    // NEVER log the raw session token.
    send_json(res, 200, {
        {"session_token", session.raw_token},
        {"user_id", user_id_db},
        {"expires_at_unix", expires_at}
    });
}

// ---------------------------------------------------------------------------
// POST /auth/logout
// ---------------------------------------------------------------------------
// Header: Authorization: Bearer <session_token>
// Response: {} 200
// ---------------------------------------------------------------------------
static void handle_logout(const httplib::Request& req,
                           httplib::Response& res,
                           Database& db,
                           tf::audit::IAuditLog& audit_log)
{
    int64_t ts = now_ms();

    auto tok = extract_bearer(req);
    if (!tok.has_value()) {
        send_json(res, 401, {{"error", "missing_token"}});
        return;
    }

    bool revoked = revoke_session(db, *tok);

    if (revoked) {
        emit_audit(audit_log, ts, "", "user", "", "", "session",
                   "session_revoked", "success", json::object());
    }

    // Always return 200 — don't leak whether the session existed.
    send_json(res, 200, json::object());
}

// ---------------------------------------------------------------------------
// GET /auth/whoami
// ---------------------------------------------------------------------------
// Header: Authorization: Bearer <session_token>
// Response: {user_id, email} or 401
// ---------------------------------------------------------------------------
static void handle_whoami(const httplib::Request& req,
                           httplib::Response& res,
                           Database& db,
                           tf::audit::IAuditLog& audit_log)
{
    int64_t ts = now_ms();
    int64_t t  = now_unix();

    auto tok = extract_bearer(req);
    if (!tok.has_value()) {
        send_json(res, 401, {{"error", "missing_token"}});
        return;
    }

    auto user_id = validate_and_touch_session(db, *tok, t);
    if (!user_id.has_value()) {
        emit_audit(audit_log, ts, "", "user", "", "", "session",
                   "session_expired", "failure", json::object());
        send_json(res, 401, {{"error", "invalid_or_expired_session"}});
        return;
    }

    // Look up email.
    std::string email_str;
    {
        auto stmt = db.prepare("SELECT email FROM users WHERE id = ?;");
        sqlite3_bind_text(stmt.get(), 1,
            user_id->data(), static_cast<int>(user_id->size()), SQLITE_STATIC);
        if (stmt.step() == SQLITE_ROW) {
            const char* e = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt.get(), 0));
            if (e) email_str = e;
        }
    }

    emit_audit(audit_log, ts, *user_id, "user", "", *user_id, "user",
               "passphrase_authentication_success", "success", json::object());

    send_json(res, 200, {
        {"user_id", *user_id},
        {"email", email_str}
    });
}

// ---------------------------------------------------------------------------
// register_auth_handlers — public entry point
// ---------------------------------------------------------------------------
void register_auth_handlers(httplib::SSLServer& server,
                              Database& db,
                              tf::audit::IAuditLog& audit_log)
{
    server.Post("/auth/enroll", [&db, &audit_log](
        const httplib::Request& req, httplib::Response& res)
    {
        try {
            handle_enroll(req, res, db, audit_log);
        } catch (const std::exception& ex) {
            // F-3: handler boundary catch — never crash the server.
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}}).dump(),
                "application/json");
        }
    });

    server.Post("/auth/login", [&db, &audit_log](
        const httplib::Request& req, httplib::Response& res)
    {
        try {
            handle_login(req, res, db, audit_log);
        } catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}}).dump(),
                "application/json");
        }
    });

    server.Post("/auth/logout", [&db, &audit_log](
        const httplib::Request& req, httplib::Response& res)
    {
        try {
            handle_logout(req, res, db, audit_log);
        } catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}}).dump(),
                "application/json");
        }
    });

    server.Get("/auth/whoami", [&db, &audit_log](
        const httplib::Request& req, httplib::Response& res)
    {
        try {
            handle_whoami(req, res, db, audit_log);
        } catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(
                json({{"error", "internal"}}).dump(),
                "application/json");
        }
    });
}

} // namespace tf::auth
