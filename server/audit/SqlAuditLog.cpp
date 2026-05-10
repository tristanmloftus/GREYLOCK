#include "SqlAuditLog.h"

#include "AuditEvent.h"
#include "CanonicalBytes.h"
#include "Sanitizer.h"

#include <sqlite3.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::audit {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

SqlAuditLog::SqlAuditLog(Database& db) : db_(db) {}

// ---------------------------------------------------------------------------
// append_locked — atomically inserts one entry into the chain.
// MUST be called with mu_ held.
// ---------------------------------------------------------------------------

void SqlAuditLog::append_locked(const AuditEvent& evt,
                                 const std::string& details_json_str) {
    // Step 1: BEGIN IMMEDIATE to serialize chain-head access.
    db_.exec("BEGIN IMMEDIATE;");
    try {
        // Step 2: SELECT the current chain head (entry_hash + seq).
        // If the table is empty, prev_hash = 32 zero bytes, seq starts at 1.
        // SQLite AUTOINCREMENT guarantees seq == last_inserted_rowid + 1 only
        // if we manage it ourselves; here we let AUTOINCREMENT handle seq and
        // we set prev_hash from the head row's entry_hash.

        std::vector<std::byte> prev_hash(32, std::byte{0});
        int64_t next_seq = 1;

        {
            auto stmt = db_.prepare(
                "SELECT seq, entry_hash FROM audit_log "
                "ORDER BY seq DESC LIMIT 1;");
            int rc = stmt.step();
            if (rc == SQLITE_ROW) {
                // Existing chain: read head seq and entry_hash.
                int64_t head_seq = sqlite3_column_int64(stmt.get(), 0);
                next_seq = head_seq + 1;

                const void* blob = sqlite3_column_blob(stmt.get(), 1);
                int blob_bytes = sqlite3_column_bytes(stmt.get(), 1);
                if (blob && blob_bytes == 32) {
                    const auto* b = static_cast<const std::byte*>(blob);
                    prev_hash.assign(b, b + 32);
                } else {
                    // Malformed chain head — treat as zero (degenerate recovery).
                    prev_hash.assign(32, std::byte{0});
                }
            }
            // rc == SQLITE_DONE means empty table; keep defaults.
        }

        // Step 3: Compute canonical bytes + entry_hash.
        PreCommitEntry pce;
        pce.seq           = next_seq;
        pce.ts_ms         = evt.ts_ms;   // F-4: multiply happens inside compute_canonical_bytes
        pce.actor_user_id = evt.actor_user_id;
        pce.actor_kind    = evt.actor_kind;
        pce.domain        = evt.domain;
        pce.subject_id    = evt.subject_id;
        pce.subject_kind  = evt.subject_kind;
        pce.action        = evt.action;
        pce.outcome       = evt.outcome;
        pce.details_json  = details_json_str;
        pce.prev_hash     = prev_hash;

        auto canonical   = compute_canonical_bytes(pce);
        auto entry_hash  = compute_entry_hash(canonical);

        // Step 4: INSERT.
        const int64_t ts_unix_nanos = evt.ts_ms * INT64_C(1'000'000);

        auto ins = db_.prepare(
            "INSERT INTO audit_log "
            "(ts_unix_nanos, actor_user_id, actor_kind, domain, subject_id, "
            " subject_kind, action, outcome, details_json, prev_hash, entry_hash) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

        sqlite3_bind_int64(ins.get(), 1, ts_unix_nanos);

        auto bind_text = [&](int idx, const std::string& s) {
            if (s.empty()) {
                sqlite3_bind_null(ins.get(), idx);
            } else {
                sqlite3_bind_text(ins.get(), idx, s.c_str(),
                                  static_cast<int>(s.size()), SQLITE_STATIC);
            }
        };
        bind_text(2, evt.actor_user_id);
        sqlite3_bind_text(ins.get(), 3, evt.actor_kind.c_str(),
                          static_cast<int>(evt.actor_kind.size()), SQLITE_STATIC);
        bind_text(4, evt.domain);
        bind_text(5, evt.subject_id);
        bind_text(6, evt.subject_kind);
        sqlite3_bind_text(ins.get(), 7, evt.action.c_str(),
                          static_cast<int>(evt.action.size()), SQLITE_STATIC);
        sqlite3_bind_text(ins.get(), 8, evt.outcome.c_str(),
                          static_cast<int>(evt.outcome.size()), SQLITE_STATIC);

        // details_json: store as BLOB
        if (details_json_str.empty()) {
            sqlite3_bind_null(ins.get(), 9);
        } else {
            sqlite3_bind_blob(ins.get(), 9, details_json_str.data(),
                              static_cast<int>(details_json_str.size()), SQLITE_STATIC);
        }

        sqlite3_bind_blob(ins.get(), 10,
                          prev_hash.data(), static_cast<int>(prev_hash.size()),
                          SQLITE_STATIC);
        sqlite3_bind_blob(ins.get(), 11,
                          entry_hash.data(), static_cast<int>(entry_hash.size()),
                          SQLITE_STATIC);

        int rc = ins.step();
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(
                std::string("SqlAuditLog: INSERT failed: ") +
                sqlite3_errmsg(db_.raw()));
        }

        // Step 5: COMMIT.
        db_.exec("COMMIT;");
    } catch (...) {
        // Step 6: ROLLBACK on any failure, then rethrow.
        char* errmsg = nullptr;
        sqlite3_exec(db_.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
        sqlite3_free(errmsg);
        throw;
    }
}

// ---------------------------------------------------------------------------
// record (public, thread-safe)
// ---------------------------------------------------------------------------

void SqlAuditLog::record(const AuditEvent& evt) {
    // Sanitize the details payload.
    SanitizerResult sr = sanitize(evt.details);

    if (sr.status == SanitizerStatus::Rejected) {
        // Log a warning to stderr (NO payload detail — only generic reason).
        std::fprintf(stderr,
            "[AUDIT WARN] sanitizer rejected payload for action=%s: %s\n",
            evt.action.c_str(), sr.reason.c_str());

        // Chain a rejection record.  Build a controlled event so we don't
        // recursively sanitize a user-supplied payload.
        AuditEvent rejection;
        rejection.ts_ms       = evt.ts_ms;
        rejection.actor_user_id = evt.actor_user_id;
        rejection.actor_kind  = evt.actor_kind;
        rejection.domain      = evt.domain;
        rejection.subject_id  = evt.subject_id;
        rejection.subject_kind = evt.subject_kind;
        rejection.action      = "audit_record_rejected";
        rejection.outcome     = "failure";
        rejection.details     = {{"kind", "sanitizer_rejected_payload"}};

        // Sanitize the controlled rejection details (should always pass).
        SanitizerResult sr2 = sanitize(rejection.details);
        std::string rejection_json = rejection.details.dump();

        std::lock_guard<std::mutex> lock(mu_);
        if (sr2.status == SanitizerStatus::Accepted) {
            try {
                append_locked(rejection, rejection_json);
            } catch (const std::exception& ex) {
                std::fprintf(stderr,
                    "[AUDIT ERROR] failed to chain rejection record: %s\n",
                    ex.what());
            }
        } else {
            // Shouldn't happen with our controlled payload; stderr only.
            std::fprintf(stderr,
                "[AUDIT ERROR] rejection record itself was rejected by sanitizer\n");
        }
        return;
    }

    // Payload passed sanitizer; serialize and append.
    std::string details_json_str = evt.details.dump();

    std::lock_guard<std::mutex> lock(mu_);
    append_locked(evt, details_json_str);
}

} // namespace tf::audit
