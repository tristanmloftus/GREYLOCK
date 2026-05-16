#pragma once

// PlaidSyncScheduler.h — Periodic Plaid transaction sync worker (Phase 4.D).
//
// DESIGN:
//   A background thread that wakes every kSyncIntervalSeconds (default 900 s)
//   and calls sync_all_accounts().  Each account with is_plaid_linked=1 is
//   processed independently via PlaidTokenBroker::withDecryptedToken so
//   plaintext tokens never escape their callback scope.
//
// GUARDRAILS:
//   F-2: access token used only inside withDecryptedToken scope.
//   F-3: per-account exceptions are caught; one bad account does not abort the sync.
//   F-4: audit timestamps computed once per event, passed as ts_ms.
//   F-5: actor_kind = "sync_worker"; no forwarded headers.
//
// SYNC STATE:
//   The plaid_sync_state table (M004) stores the Plaid /transactions/sync
//   cursor per account, plus the last_sync_unix timestamp.
//
// THREAD SAFETY:
//   start() / stop() must be called from the same thread (typically main).
//   sync_all_accounts() may be called directly from tests.

#include "../db/Database.h"
#include "../audit/IAuditLog.h"
#include "../audit/AuditEvent.h"
#include "PlaidApiClient.h"
#include "PlaidTokenBroker.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace tf::plaid {

class PlaidSyncScheduler {
public:
    // Construct the scheduler.
    //
    // db:     Database reference (must outlive this object).
    // broker: PlaidTokenBroker reference (must outlive this object).
    // api:    PlaidApiClient reference (must outlive this object).
    // audit:  IAuditLog reference (must outlive this object).
    //
    // sync_interval_seconds: how often to run sync (default 900 = 15 min).
    //   Override via TF_PLAID_SYNC_INTERVAL_SECONDS env var, or set directly
    //   in tests by passing a value.
    explicit PlaidSyncScheduler(
        Database& db,
        PlaidTokenBroker& broker,
        PlaidApiClient& api,
        tf::audit::IAuditLog& audit,
        int64_t sync_interval_seconds = 900
    );

    ~PlaidSyncScheduler();

    // Non-copyable, non-movable.
    PlaidSyncScheduler(const PlaidSyncScheduler&) = delete;
    PlaidSyncScheduler& operator=(const PlaidSyncScheduler&) = delete;
    PlaidSyncScheduler(PlaidSyncScheduler&&) = delete;
    PlaidSyncScheduler& operator=(PlaidSyncScheduler&&) = delete;

    // start() — spawns the background worker thread.
    // The thread runs: wait interval, sync, repeat.
    // Must not be called if already started.
    void start();

    // stop() — signals the worker thread to stop and joins it.
    // Safe to call before start() (no-op).
    void stop();

    // sync_all_accounts() — public so tests can call it directly without
    // waiting for a timer tick.  Thread-safe only if the caller ensures no
    // concurrent calls (in tests, call from a single test thread).
    void sync_all_accounts();

    // sync_account() — force-sync a single account by ID.
    // Decrypts the token, calls /transactions/sync, persists results.
    // Throws on account-not-found or decryption failure.
    // Thread-safe only if caller ensures no concurrent calls for the same id.
    void sync_account(const std::string& account_id);

private:
    // Worker thread body.
    void worker_thread();

    // Retrieve the stored cursor for account_id, or nullopt if none.
    std::optional<std::string> get_cursor(const std::string& account_id);

    // Persist the cursor and update last_sync_unix.
    void store_cursor(const std::string& account_id,
                      const std::string& cursor);

    // Emit a sync audit event.
    void emit_sync_audit(const std::string& account_id,
                         const std::string& action,
                         const std::string& outcome,
                         const nlohmann::json& details);

    // INSERT or UPDATE a transaction row from a PlaidTransaction.
    void upsert_transaction(const std::string& account_id,
                            const PlaidTransaction& tx);

    // UPDATE a transaction row (for modified transactions).
    void update_transaction(const PlaidTransaction& tx);

    // DELETE a transaction row by plaid_transaction_id.
    void delete_transaction(const std::string& plaid_tx_id);

    Database&             db_;
    PlaidTokenBroker&     broker_;
    PlaidApiClient&       api_;
    tf::audit::IAuditLog& audit_;

    int64_t  sync_interval_seconds_;
    std::atomic<bool> stop_flag_{false};
    std::thread worker_thread_;
    bool      started_{false};
};

} // namespace tf::plaid
