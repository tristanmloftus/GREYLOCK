#pragma once

// SqlAuditLog.h — BLAKE2b-chained audit log writer (Phase 4).
//
// Implements IAuditLog against the `audit_log` SQLite table created by M001.
//
// Thread safety: record() is safe to call concurrently.  SQLite's default
// serialized mode handles concurrent access; the BEGIN IMMEDIATE transaction
// guarantees the chain-head SELECT and subsequent INSERT are atomic.
//
// Sanitizer rejection policy: when record() receives a payload that the
// sanitizer rejects, it logs a warning to stderr (no payload detail) and
// records an `audit_record_rejected` chain entry with
// details_json = {"kind":"sanitizer_rejected_payload"}.  The rejection itself
// is chained so no gap appears in the audit sequence.
//
// On sanitizer rejection of the rejection record itself (shouldn't happen given
// the controlled details), we fall back to stderr-only logging to avoid infinite
// recursion.

#include "IAuditLog.h"
#include "../db/Database.h"

#include <mutex>

namespace tf::audit {

class SqlAuditLog : public IAuditLog {
public:
    // db must outlive this object.
    explicit SqlAuditLog(Database& db);
    ~SqlAuditLog() override = default;

    // Thread-safe.  Runs the sanitizer, computes canonical bytes, hashes, and
    // inserts inside a BEGIN IMMEDIATE transaction.
    void record(const AuditEvent& evt) override;

private:
    Database& db_;
    std::mutex mu_; // serializes BEGIN IMMEDIATE blocks for thread safety

    // Internal: append an already-sanitized + serialized event to the chain.
    // Called with mu_ held.
    void append_locked(const AuditEvent& evt, const std::string& details_json_str);
};

} // namespace tf::audit
