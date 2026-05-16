// PlaidSyncScheduler.cpp — Periodic Plaid transaction sync worker (Phase 4.D).
//
// See PlaidSyncScheduler.h for design notes, guardrails, and security contract.

#include "PlaidSyncScheduler.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sodium.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace tf::plaid {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
PlaidSyncScheduler::PlaidSyncScheduler(
    Database& db,
    PlaidTokenBroker& broker,
    PlaidApiClient& api,
    tf::audit::IAuditLog& audit,
    int64_t sync_interval_seconds)
    : db_(db)
    , broker_(broker)
    , api_(api)
    , audit_(audit)
    , sync_interval_seconds_(sync_interval_seconds)
{
    // Allow override via env var.
    const char* env_interval = std::getenv("TF_PLAID_SYNC_INTERVAL_SECONDS");
    if (env_interval && env_interval[0] != '\0') {
        try {
            int64_t v = std::stoll(env_interval);
            if (v > 0) {
                sync_interval_seconds_ = v;
            }
        } catch (...) {
            // Invalid env var — use the constructor default.
        }
    }
}

// ---------------------------------------------------------------------------
// Destructor — stop the thread if still running.
// ---------------------------------------------------------------------------
PlaidSyncScheduler::~PlaidSyncScheduler() {
    stop();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::start() {
    if (started_) return;
    stop_flag_.store(false);
    started_ = true;
    worker_thread_ = std::thread(&PlaidSyncScheduler::worker_thread, this);
    std::cerr << "PlaidSyncScheduler: started, interval="
              << sync_interval_seconds_ << "s\n";
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::stop() {
    if (!started_) return;
    stop_flag_.store(true);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    started_ = false;
    std::cerr << "PlaidSyncScheduler: stopped\n";
}

// ---------------------------------------------------------------------------
// worker_thread — runs in the background thread.
//
// Pattern: sleep → sync → repeat.  The sleep is broken into short increments
// so stop() doesn't wait up to sync_interval_seconds_ to join.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::worker_thread() {
    // Wait the full interval before the first sync to avoid a burst at startup.
    int64_t elapsed = 0;
    while (elapsed < sync_interval_seconds_) {
        if (stop_flag_.load()) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++elapsed;
    }

    while (!stop_flag_.load()) {
        try {
            sync_all_accounts();
        } catch (const std::exception& ex) {
            // Per-account errors are handled inside sync_all_accounts;
            // this catches unexpected scheduler-level failures.
            std::cerr << "PlaidSyncScheduler: unexpected error in sync cycle: "
                      << ex.what() << "\n";
        } catch (...) {
            std::cerr << "PlaidSyncScheduler: unknown error in sync cycle\n";
        }

        // Sleep in 1-second increments so stop() is responsive.
        int64_t waited = 0;
        while (waited < sync_interval_seconds_) {
            if (stop_flag_.load()) return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++waited;
        }
    }
}

// ---------------------------------------------------------------------------
// sync_account — force-sync a single Plaid-linked account.
//
// Decrypts the access token, calls /transactions/sync, persists results.
// Throws on account-not-found or decryption failure; reports API errors
// via audit log without throwing.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::sync_account(const std::string& account_id) {
    // Verify account exists.
    {
        auto stmt = db_.prepare(
            "SELECT is_plaid_linked FROM accounts WHERE id = ?;");
        sqlite3_bind_text(stmt.get(), 1,
            account_id.c_str(), static_cast<int>(account_id.size()), SQLITE_STATIC);
        if (stmt.step() != SQLITE_ROW) {
            throw std::runtime_error(
                "PlaidSyncScheduler::sync_account: account not found: " + account_id);
        }
        if (sqlite3_column_int(stmt.get(), 0) == 0) {
            throw std::runtime_error(
                "PlaidSyncScheduler::sync_account: account not linked: " + account_id);
        }
    }

    broker_.withDecryptedToken(
        account_id,
        [&](std::string_view token) -> void {
            auto cursor_opt = get_cursor(account_id);
            auto result = api_.sync_transactions(token, cursor_opt);
            if (!result.has_value()) {
                emit_sync_audit(account_id, "plaid_sync_failed", "failure",
                    {{"reason", "api_error"}});
                return;
            }

            int added_count = 0;
            for (const auto& tx : result->added) {
                try { upsert_transaction(account_id, tx); ++added_count; }
                catch (const std::exception& ex) {
                    std::cerr << "PlaidSyncScheduler: insert failed for tx "
                              << tx.transaction_id << ": " << ex.what() << "\n";
                }
            }

            int modified_count = 0;
            for (const auto& tx : result->modified) {
                try { update_transaction(tx); ++modified_count; }
                catch (const std::exception& ex) {
                    std::cerr << "PlaidSyncScheduler: update failed for tx "
                              << tx.transaction_id << ": " << ex.what() << "\n";
                }
            }

            int removed_count = 0;
            for (const auto& tx_id : result->removed_ids) {
                try { delete_transaction(tx_id); ++removed_count; }
                catch (const std::exception& ex) {
                    std::cerr << "PlaidSyncScheduler: delete failed for tx "
                              << tx_id << ": " << ex.what() << "\n";
                }
            }

            if (!result->next_cursor.empty()) {
                store_cursor(account_id, result->next_cursor);
            }

            // Persist the current balance from the sync response.
            // Single-account-per-item model: use the first non-empty
            // balance Plaid returned.  Multi-account-per-item is a v2
            // problem (see greylock-kickoff.md §4.4).
            int64_t balance_cents_updated = 0;
            if (!result->accounts.empty()) {
                const int64_t cents = result->accounts.front().current_cents;
                auto upd = db_.prepare(
                    "UPDATE accounts SET balance_cents = ? WHERE id = ?;");
                sqlite3_bind_int64(upd.get(), 1, cents);
                sqlite3_bind_text(upd.get(), 2,
                    account_id.c_str(),
                    static_cast<int>(account_id.size()), SQLITE_STATIC);
                if (upd.step() == SQLITE_DONE) {
                    balance_cents_updated = cents;
                }
            }

            emit_sync_audit(account_id, "plaid_sync_completed", "success",
                {{"added", added_count},
                 {"modified", modified_count},
                 {"removed", removed_count},
                 {"balance_cents", balance_cents_updated}});
        },
        [&]() -> void {
            emit_sync_audit(account_id, "plaid_sync_failed", "failure",
                {{"reason", "token_missing"}});
        }
    );
}

// ---------------------------------------------------------------------------
// sync_all_accounts
//
// 1. SELECT all Plaid-linked account IDs.
// 2. For each, call sync_account (per-account error isolation — F-3).
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::sync_all_accounts() {
    std::vector<std::string> account_ids;
    {
        auto stmt = db_.prepare(
            "SELECT id FROM accounts WHERE is_plaid_linked = 1;");
        while (stmt.step() == SQLITE_ROW) {
            const char* id = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt.get(), 0));
            if (id) {
                account_ids.emplace_back(id);
            }
        }
    }

    if (account_ids.empty()) {
        return;
    }

    for (const std::string& account_id : account_ids) {
        try {
            sync_account(account_id);
        } catch (const std::exception& ex) {
            std::cerr << "PlaidSyncScheduler: error processing account "
                      << account_id << ": " << ex.what() << "\n";
            try {
                emit_sync_audit(account_id, "plaid_sync_failed", "failure",
                    {{"reason", "exception"}, {"message", ex.what()}});
            } catch (...) {
                std::cerr << "PlaidSyncScheduler: failed to write audit for "
                          << account_id << "\n";
            }
        } catch (...) {
            std::cerr << "PlaidSyncScheduler: unknown exception processing account "
                      << account_id << "\n";
            try {
                emit_sync_audit(account_id, "plaid_sync_failed", "failure",
                    {{"reason", "unknown_exception"}});
            } catch (...) {}
        }
    }
}

// ---------------------------------------------------------------------------
// get_cursor — SELECT cursor FROM plaid_sync_state WHERE account_id = ?
// ---------------------------------------------------------------------------
std::optional<std::string> PlaidSyncScheduler::get_cursor(
    const std::string& account_id)
{
    try {
        auto stmt = db_.prepare(
            "SELECT cursor FROM plaid_sync_state WHERE account_id = ?;");
        sqlite3_bind_text(stmt.get(), 1,
            account_id.c_str(), static_cast<int>(account_id.size()),
            SQLITE_STATIC);
        if (stmt.step() == SQLITE_ROW) {
            const char* cur = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt.get(), 0));
            if (cur && cur[0] != '\0') {
                return std::string(cur);
            }
        }
    } catch (...) {
        // Table may not exist yet (before M004 applied) — return nullopt.
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// store_cursor — upsert into plaid_sync_state
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::store_cursor(const std::string& account_id,
                                      const std::string& cursor)
{
    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto stmt = db_.prepare(
        "INSERT INTO plaid_sync_state (account_id, cursor, last_sync_unix) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(account_id) DO UPDATE SET "
        "  cursor = excluded.cursor, "
        "  last_sync_unix = excluded.last_sync_unix;");

    sqlite3_bind_text(stmt.get(), 1,
        account_id.c_str(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2,
        cursor.c_str(), static_cast<int>(cursor.size()), SQLITE_STATIC);
    sqlite3_bind_int64(stmt.get(), 3, now);

    stmt.step();  // SQLITE_DONE on success
}

// ---------------------------------------------------------------------------
// emit_sync_audit — build and record an audit event for the sync worker.
// actor_kind = "sync_worker".  F-4: ts_ms computed once.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::emit_sync_audit(const std::string& account_id,
                                          const std::string& action,
                                          const std::string& outcome,
                                          const nlohmann::json& details)
{
    int64_t ts_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    tf::audit::AuditEvent evt;
    evt.ts_ms         = ts_ms;
    evt.actor_user_id = "";  // system actor
    evt.actor_kind    = "sync_worker";
    evt.domain        = "";
    evt.subject_id    = account_id;
    evt.subject_kind  = "account";
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;

    audit_.record(evt);
}

// ---------------------------------------------------------------------------
// upsert_transaction — INSERT OR REPLACE a Plaid transaction into the
// transactions table.
//
// Generates a stable internal ID derived from the plaid_transaction_id so
// that re-syncing the same transaction is idempotent.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::upsert_transaction(const std::string& account_id,
                                             const PlaidTransaction& tx)
{
    // Generate a deterministic internal ID from the plaid_transaction_id.
    // We use BLAKE2b (32 bytes) over the plaid_transaction_id bytes.
    // This ensures the same Plaid transaction always maps to the same row.
    uint8_t hash_bytes[32];
    crypto_generichash(
        hash_bytes, sizeof(hash_bytes),
        reinterpret_cast<const uint8_t*>(tx.transaction_id.data()),
        tx.transaction_id.size(),
        nullptr, 0);

    char id_hex[65];
    sodium_bin2hex(id_hex, sizeof(id_hex), hash_bytes, sizeof(hash_bytes));
    std::string internal_id(id_hex);

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // INSERT OR REPLACE so duplicate syncs are idempotent.
    //
    // description_encrypted: v0.2 stores Plaid's tx.name (merchant /
    // description) as UTF-8 bytes in the BLOB column. The "_encrypted"
    // suffix is aspirational — Phase 4.C wraps with envelope encryption.
    auto stmt = db_.prepare(
        "INSERT INTO transactions "
        "  (id, account_id, plaid_transaction_id, posted_at_unix, "
        "   amount_cents, description_encrypted, category, created_at_unix) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  amount_cents = excluded.amount_cents, "
        "  posted_at_unix = excluded.posted_at_unix, "
        "  description_encrypted = excluded.description_encrypted, "
        "  category = excluded.category;");

    sqlite3_bind_text(stmt.get(), 1,
        internal_id.c_str(), static_cast<int>(internal_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2,
        account_id.c_str(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 3,
        tx.transaction_id.c_str(), static_cast<int>(tx.transaction_id.size()), SQLITE_STATIC);
    sqlite3_bind_int64(stmt.get(), 4, tx.date_unix);
    sqlite3_bind_int64(stmt.get(), 5, tx.amount_cents);
    if (tx.name.empty()) {
        sqlite3_bind_null(stmt.get(), 6);
    } else {
        sqlite3_bind_blob(stmt.get(), 6,
            tx.name.data(), static_cast<int>(tx.name.size()), SQLITE_TRANSIENT);
    }
    if (tx.category.empty()) {
        sqlite3_bind_null(stmt.get(), 7);
    } else {
        sqlite3_bind_text(stmt.get(), 7,
            tx.category.c_str(), static_cast<int>(tx.category.size()), SQLITE_STATIC);
    }
    sqlite3_bind_int64(stmt.get(), 8, now);

    int rc = stmt.step();
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(
            "PlaidSyncScheduler::upsert_transaction: INSERT failed for plaid_tx_id=" +
            tx.transaction_id);
    }
}

// ---------------------------------------------------------------------------
// update_transaction — UPDATE a transaction row by plaid_transaction_id.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::update_transaction(const PlaidTransaction& tx) {
    auto stmt = db_.prepare(
        "UPDATE transactions SET "
        "  amount_cents = ?, "
        "  posted_at_unix = ?, "
        "  description_encrypted = ?, "
        "  category = ? "
        "WHERE plaid_transaction_id = ?;");

    sqlite3_bind_int64(stmt.get(), 1, tx.amount_cents);
    sqlite3_bind_int64(stmt.get(), 2, tx.date_unix);
    if (tx.name.empty()) {
        sqlite3_bind_null(stmt.get(), 3);
    } else {
        sqlite3_bind_blob(stmt.get(), 3,
            tx.name.data(), static_cast<int>(tx.name.size()), SQLITE_TRANSIENT);
    }
    if (tx.category.empty()) {
        sqlite3_bind_null(stmt.get(), 4);
    } else {
        sqlite3_bind_text(stmt.get(), 4,
            tx.category.c_str(), static_cast<int>(tx.category.size()), SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt.get(), 5,
        tx.transaction_id.c_str(), static_cast<int>(tx.transaction_id.size()), SQLITE_STATIC);

    stmt.step();
}

// ---------------------------------------------------------------------------
// delete_transaction — DELETE a transaction row by plaid_transaction_id.
// ---------------------------------------------------------------------------
void PlaidSyncScheduler::delete_transaction(const std::string& plaid_tx_id) {
    auto stmt = db_.prepare(
        "DELETE FROM transactions WHERE plaid_transaction_id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        plaid_tx_id.c_str(), static_cast<int>(plaid_tx_id.size()), SQLITE_STATIC);
    stmt.step();
}

} // namespace tf::plaid
