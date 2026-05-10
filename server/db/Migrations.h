#pragma once

#include "Database.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Migration — a single versioned schema change.
//
// Fields:
//   version  — monotonically increasing integer (1, 2, 3, …).
//   name     — human-readable label stored in schema_migrations.
//   up       — C++ function that applies the migration using db.exec() calls.
//              Called inside a BEGIN IMMEDIATE / COMMIT transaction by
//              Migrations::apply_pending().  If up() throws, the transaction
//              is rolled back and the migration is NOT recorded as applied.
// ---------------------------------------------------------------------------
struct Migration {
    int version;
    std::string name;
    std::function<void(Database&)> up;
};

// ---------------------------------------------------------------------------
// Migrations — migration runner.
//
// Usage:
//   Migrations migrations;
//   migrations.register_migration({1, "M001_initial_schema", M001_up});
//   migrations.apply_pending(db);
//
// apply_pending():
//   1. Creates schema_migrations table if absent.
//   2. Queries the highest applied version.
//   3. For each pending migration (version > current, ascending order):
//      a. BEGIN IMMEDIATE
//      b. Call migration.up(db)
//      c. INSERT INTO schema_migrations (version, name, applied_at_unix) ...
//      d. COMMIT
//      On any exception: ROLLBACK, stop, rethrow.
//
// current_version():
//   Returns the highest applied version from schema_migrations, or 0 if the
//   table does not exist or is empty.
//
// Thread safety: Migrations is NOT thread-safe.  External synchronization
// required if apply_pending() / current_version() are called concurrently.
// ---------------------------------------------------------------------------
class Migrations {
public:
    Migrations() = default;

    // Register a migration.  Migrations must be registered in ascending
    // version order.  Duplicate versions throw std::invalid_argument.
    void register_migration(Migration m);

    // Apply all pending migrations to db.  Creates schema_migrations table if
    // absent.  Each migration runs in its own IMMEDIATE transaction.
    // Throws std::runtime_error if any migration fails (the failed migration's
    // transaction is rolled back; already-applied migrations are not undone).
    void apply_pending(Database& db);

    // Returns the highest applied version, or 0 if no migrations have been applied.
    // Creates schema_migrations if absent (idempotent read).
    int current_version(Database& db);

private:
    // Ensure schema_migrations table exists.  Safe to call multiple times.
    static void ensure_migrations_table(Database& db);

    std::vector<Migration> migrations_;
};

// ---------------------------------------------------------------------------
// M001_initial_schema — creates the 8 application tables.
// Called by Migrations::apply_pending() inside a transaction.
// Exposed here so tests can reference it directly and server/main.cpp can
// register it without a separate translation unit dependency.
// ---------------------------------------------------------------------------
void M001_initial_schema_up(Database& db);

// ---------------------------------------------------------------------------
// M002_categories_table — adds the categories table (Phase 4.B).
// ---------------------------------------------------------------------------
void M002_categories_table_up(Database& db);

// ---------------------------------------------------------------------------
// M003_budgets_table — adds the budgets table (Phase 4.B).
// ---------------------------------------------------------------------------
void M003_budgets_table_up(Database& db);

// ---------------------------------------------------------------------------
// M004_plaid_sync_state — adds the plaid_sync_state table (Phase 4.D).
//
// Stores the Plaid /transactions/sync cursor per account so incremental
// syncs fetch only new deltas.
// ---------------------------------------------------------------------------
void M004_plaid_sync_state_up(Database& db);
