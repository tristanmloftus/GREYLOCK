#include "Migrations.h"
#include "Database.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Migrations::ensure_migrations_table
// ---------------------------------------------------------------------------
void Migrations::ensure_migrations_table(Database& db) {
    db.exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version          INTEGER PRIMARY KEY,"
        "  name             TEXT    NOT NULL,"
        "  applied_at_unix  INTEGER NOT NULL"
        ");"
    );
}

// ---------------------------------------------------------------------------
// Migrations::register_migration
// ---------------------------------------------------------------------------
void Migrations::register_migration(Migration m) {
    // Check for duplicate version.
    for (const auto& existing : migrations_) {
        if (existing.version == m.version) {
            throw std::invalid_argument(
                "Migrations::register_migration: duplicate version " +
                std::to_string(m.version));
        }
    }
    migrations_.push_back(std::move(m));
    // Keep sorted by version ascending.
    std::sort(migrations_.begin(), migrations_.end(),
              [](const Migration& a, const Migration& b) {
                  return a.version < b.version;
              });
}

// ---------------------------------------------------------------------------
// Migrations::current_version
// ---------------------------------------------------------------------------
int Migrations::current_version(Database& db) {
    ensure_migrations_table(db);
    auto stmt = db.prepare(
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;");
    int rc = stmt.step();
    if (rc != SQLITE_ROW) {
        throw std::runtime_error(
            "Migrations::current_version: unexpected step result " +
            std::to_string(rc));
    }
    return sqlite3_column_int(stmt.get(), 0);
}

// ---------------------------------------------------------------------------
// Migrations::apply_pending
// ---------------------------------------------------------------------------
void Migrations::apply_pending(Database& db) {
    ensure_migrations_table(db);
    int current = current_version(db);

    for (const auto& migration : migrations_) {
        if (migration.version <= current) {
            continue;  // already applied
        }

        // Begin an IMMEDIATE transaction so concurrent writers are blocked
        // for the duration of this migration.
        db.exec("BEGIN IMMEDIATE;");
        try {
            migration.up(db);

            // Record the migration as applied.
            int64_t now = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count());

            auto ins = db.prepare(
                "INSERT INTO schema_migrations (version, name, applied_at_unix) "
                "VALUES (?, ?, ?);");
            sqlite3_bind_int(ins.get(), 1, migration.version);
            sqlite3_bind_text(ins.get(), 2, migration.name.c_str(),
                              static_cast<int>(migration.name.size()),
                              SQLITE_STATIC);
            sqlite3_bind_int64(ins.get(), 3, now);
            int rc = ins.step();
            if (rc != SQLITE_DONE) {
                throw std::runtime_error(
                    "Migrations::apply_pending: INSERT into schema_migrations failed "
                    "for version " + std::to_string(migration.version));
            }

            db.exec("COMMIT;");
        } catch (...) {
            // Best-effort rollback.  sqlite3_exec handles the case where no
            // transaction is active (e.g. an auto-commit DDL statement closed it).
            char* errmsg = nullptr;
            sqlite3_exec(db.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
            sqlite3_free(errmsg);
            throw;  // propagate the original exception
        }
    }
}

// ---------------------------------------------------------------------------
// M002_categories_table_up — Phase 4.B
//
// M001 did not include categories or budgets tables.  They are added here as
// separate numbered migrations so existing databases can be upgraded without
// re-running the initial schema.
// ---------------------------------------------------------------------------
void M002_categories_table_up(Database& db) {
    db.exec(
        "CREATE TABLE categories ("
        "  id         TEXT NOT NULL PRIMARY KEY,"
        "  entity_id  TEXT NOT NULL,"
        "  name       TEXT NOT NULL,"
        "  kind       TEXT NOT NULL,"
        "  FOREIGN KEY (entity_id) REFERENCES entities(id)"
        ");"
    );
}

// ---------------------------------------------------------------------------
// M003_budgets_table_up — Phase 4.B
// ---------------------------------------------------------------------------
void M003_budgets_table_up(Database& db) {
    db.exec(
        "CREATE TABLE budgets ("
        "  id                TEXT    NOT NULL PRIMARY KEY,"
        "  entity_id         TEXT    NOT NULL,"
        "  category_id       TEXT,"
        "  amount_cents      INTEGER NOT NULL DEFAULT 0,"
        "  period_start_unix INTEGER NOT NULL,"
        "  period_end_unix   INTEGER NOT NULL,"
        "  FOREIGN KEY (entity_id) REFERENCES entities(id)"
        ");"
    );
}

// ---------------------------------------------------------------------------
// M001_initial_schema_up
//
// Creates the 8 application tables.  schema_migrations is NOT created here —
// the migration runner creates it before calling any migration.
//
// Column-type rationale:
//   - IDs as TEXT (UUIDs): cross-machine sync arrives in Phase 4; UUID strings
//     survive any byte-order issue.
//   - Amounts as INTEGER (cents): avoids floating-point representation issues.
//   - Timestamps as INTEGER (Unix seconds): simple, sortable, no timezone issues.
//   - Sensitive blobs (passphrase_hash, totp_secret, encrypted_token,
//     description_encrypted, audit entry hashes) as BLOB: Phase 4 fills them
//     with opaque ciphertext.  BLOB preserves byte identity; TEXT would silently
//     mangle non-UTF-8 bytes.
//
// Foreign-key enforcement is enabled by Database's constructor via
// PRAGMA foreign_keys = ON.
// ---------------------------------------------------------------------------
void M001_initial_schema_up(Database& db) {
    // ------------------------------------------------------------------
    // users
    //   passphrase_hash and totp_secret are placeholder columns for Phase 3
    //   (Argon2id + TOTP).  For v0.2, they are declared but never populated.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE users ("
        "  id                TEXT    PRIMARY KEY,"
        "  email             TEXT    NOT NULL UNIQUE,"
        "  created_at_unix   INTEGER NOT NULL,"
        "  passphrase_hash   BLOB    NOT NULL,"
        "  totp_secret       BLOB"
        ");"
    );

    // ------------------------------------------------------------------
    // entities
    //   kind is constrained to the six values from the plan.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE entities ("
        "  id               TEXT    PRIMARY KEY,"
        "  name             TEXT    NOT NULL,"
        "  kind             TEXT    NOT NULL"
        "      CHECK(kind IN ('Individual','LLC','Corporation',"
        "                     'Partnership','Trust','Other')),"
        "  created_at_unix  INTEGER NOT NULL"
        ");"
    );

    // ------------------------------------------------------------------
    // entity_memberships
    //   Composite PK; FK to both users and entities.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE entity_memberships ("
        "  user_id    TEXT NOT NULL,"
        "  entity_id  TEXT NOT NULL,"
        "  role       TEXT NOT NULL,"
        "  PRIMARY KEY (user_id, entity_id),"
        "  FOREIGN KEY (user_id)   REFERENCES users(id),"
        "  FOREIGN KEY (entity_id) REFERENCES entities(id)"
        ");"
    );

    // ------------------------------------------------------------------
    // accounts
    //   encrypted_token stays NULL until Phase 4.
    //   is_plaid_linked uses INTEGER (0/1) per SQLite boolean convention.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE accounts ("
        "  id               TEXT    PRIMARY KEY,"
        "  entity_id        TEXT    NOT NULL,"
        "  name             TEXT    NOT NULL,"
        "  kind             TEXT    NOT NULL,"
        "  balance_cents    INTEGER NOT NULL DEFAULT 0,"
        "  plaid_item_id    TEXT,"
        "  plaid_account_id TEXT,"
        "  encrypted_token  BLOB,"
        "  is_plaid_linked  INTEGER NOT NULL DEFAULT 0,"
        "  created_at_unix  INTEGER NOT NULL,"
        "  FOREIGN KEY (entity_id) REFERENCES entities(id)"
        ");"
    );

    // ------------------------------------------------------------------
    // transactions
    //   description_encrypted is BLOB (Phase 4 will encrypt transaction
    //   descriptions via envelope encryption).  Plan §c-A explicitly says
    //   server-side queries operate on plaintext fields (amount, date, category);
    //   description is encrypted client-side.
    //   Index on (account_id, posted_at_unix) for time-range queries.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE transactions ("
        "  id                      TEXT    PRIMARY KEY,"
        "  account_id              TEXT    NOT NULL,"
        "  plaid_transaction_id    TEXT,"
        "  posted_at_unix          INTEGER NOT NULL,"
        "  amount_cents            INTEGER NOT NULL,"
        "  description_encrypted   BLOB,"
        "  category                TEXT,"
        "  created_at_unix         INTEGER NOT NULL,"
        "  FOREIGN KEY (account_id) REFERENCES accounts(id)"
        ");"
    );
    db.exec(
        "CREATE INDEX idx_transactions_account_posted"
        "  ON transactions (account_id, posted_at_unix);"
    );

    // ------------------------------------------------------------------
    // audit_log
    //   prev_hash and entry_hash are BLAKE2b-256 digests (Phase 4).
    //   For v0.2 they default to zero-length blobs — the columns are
    //   declared NOT NULL because the chain must always record a hash.
    //   seq is AUTOINCREMENT so gaps are never reused (important for
    //   the hash-chain integrity guarantee).
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE audit_log ("
        "  seq             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts_unix_nanos   INTEGER NOT NULL,"
        "  actor_user_id   TEXT,"
        "  actor_kind      TEXT    NOT NULL,"
        "  domain          TEXT,"
        "  subject_id      TEXT,"
        "  subject_kind    TEXT,"
        "  action          TEXT    NOT NULL,"
        "  outcome         TEXT    NOT NULL,"
        "  details_json    BLOB,"
        "  prev_hash       BLOB    NOT NULL,"
        "  entry_hash      BLOB    NOT NULL"
        ");"
    );

    // ------------------------------------------------------------------
    // sessions
    //   revoked uses INTEGER (0/1).  Phase 3 populates this table.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE sessions ("
        "  id               TEXT    PRIMARY KEY,"
        "  user_id          TEXT    NOT NULL,"
        "  created_at_unix  INTEGER NOT NULL,"
        "  last_seen_unix   INTEGER NOT NULL,"
        "  expires_at_unix  INTEGER NOT NULL,"
        "  revoked          INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY (user_id) REFERENCES users(id)"
        ");"
    );

    // ------------------------------------------------------------------
    // enrollment_tokens
    //   token_hash is the hashed one-shot enrollment token (Phase 3).
    //   consumed_at_unix is NULL until the token is redeemed.
    // ------------------------------------------------------------------
    db.exec(
        "CREATE TABLE enrollment_tokens ("
        "  token_hash        BLOB    PRIMARY KEY,"
        "  email             TEXT    NOT NULL,"
        "  created_at_unix   INTEGER NOT NULL,"
        "  expires_at_unix   INTEGER NOT NULL,"
        "  consumed_at_unix  INTEGER"
        ");"
    );
}
