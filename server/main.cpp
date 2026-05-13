#include "http/Server.h"
#include "db/Database.h"
#include "db/Migrations.h"
#include "auth/AuthHandlers.h"
#include "auth/EnrollmentToken.h"
#include "audit/SqlAuditLog.h"
#include "data/EntitiesHandler.h"
#include "data/AccountsHandler.h"
#include "data/TransactionsHandler.h"
#include "data/CategoriesHandler.h"
#include "data/BudgetsHandler.h"
#include "discovery/SupplierMapHandler.h"
#include "plaid/PlaidTokenBroker.h"
#include "plaid/PlaidApiClient.h"
#include "plaid/PlaidSyncScheduler.h"
#include "../src/services/http/CurlHttpClient.h"

#include <sodium.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <csignal>

// --------------------------------------------------------------------------
// Global server pointer for SIGINT/SIGTERM handler.
// --------------------------------------------------------------------------
static Server* g_server{nullptr};

static void signal_handler(int /*sig*/) {
    if (g_server) {
        g_server->stop();
    }
}

// --------------------------------------------------------------------------
// Config helpers: read env var or return default.
// --------------------------------------------------------------------------
static std::string env_or(const char* name, const char* fallback) {
    const char* val = std::getenv(name);
    return (val && val[0] != '\0') ? std::string(val) : std::string(fallback);
}

static uint16_t env_port(const char* name, uint16_t fallback) {
    const char* val = std::getenv(name);
    if (!val || val[0] == '\0') return fallback;
    try {
        long p = std::stol(val);
        if (p < 1 || p > 65535) {
            std::cerr << "Warning: " << name << "=" << val
                      << " out of valid port range [1,65535]; using default "
                      << fallback << "\n";
            return fallback;
        }
        return static_cast<uint16_t>(p);
    } catch (...) {
        std::cerr << "Warning: " << name << "=" << val
                  << " is not a valid port; using default " << fallback << "\n";
        return fallback;
    }
}

// --------------------------------------------------------------------------
// read_master_key: reads TF_MASTER_KEY from the environment.
//
// Returns std::nullopt if the env var is unset or empty.
// Returns the hex string if set.
// Does NOT validate format here — Database constructor does that.
//
// GUARDRAIL: do NOT log the returned value.
// --------------------------------------------------------------------------
static std::optional<std::string> read_master_key() {
    const char* val = std::getenv("TF_MASTER_KEY");
    if (!val || val[0] == '\0') {
        return std::nullopt;
    }
    return std::string(val);
}

// --------------------------------------------------------------------------
// open_db_with_migrations: shared helper for both admin CLI and server boot.
//
// master_key_hex: passed through to Database constructor.  See Database.h
// for key format requirements (64 hex chars = 32 bytes).
// --------------------------------------------------------------------------
static Database open_db_with_migrations(const std::string& db_path,
                                        std::optional<std::string>&& master_key_hex) {
    {
        std::filesystem::path db_file_path(db_path);
        if (db_file_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(db_file_path.parent_path(), ec);
            if (ec) {
                throw std::runtime_error(
                    "Could not create database directory '" +
                    db_file_path.parent_path().string() +
                    "': " + ec.message());
            }
        }
    }

    Database db(db_path, std::move(master_key_hex));

    Migrations migrations;
    migrations.register_migration({1, "M001_initial_schema",     M001_initial_schema_up});
    migrations.register_migration({2, "M002_categories_table",   M002_categories_table_up});
    migrations.register_migration({3, "M003_budgets_table",      M003_budgets_table_up});
    migrations.register_migration({4, "M004_plaid_sync_state",   M004_plaid_sync_state_up});
    migrations.apply_pending(db);

    return db;
}

// --------------------------------------------------------------------------
// print_users: used by --list-users admin command.
// --------------------------------------------------------------------------
static void print_users(Database& db) {
    auto stmt = db.prepare(
        "SELECT id, email, created_at_unix FROM users ORDER BY created_at_unix;"
    );
    std::cout << std::left
              << std::setw(36) << "ID"
              << "  "
              << std::setw(40) << "EMAIL"
              << "  "
              << "CREATED_AT_UNIX\n";
    std::cout << std::string(90, '-') << "\n";
    while (stmt.step() == SQLITE_ROW) {
        const char* id    = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const char* email = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        int64_t created   = sqlite3_column_int64(stmt.get(), 2);
        std::cout << std::setw(36) << (id    ? id    : "")
                  << "  "
                  << std::setw(40) << (email ? email : "")
                  << "  "
                  << created << "\n";
    }
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Initialize libsodium.  Must be called before any sodium_* function.
    if (sodium_init() < 0) {
        std::cerr << "ERROR: sodium_init() failed\n";
        return 1;
    }

    // Parse arguments for admin CLI.
    std::string mint_email;
    int64_t     mint_ttl_secs = 24 * 3600; // default 24 h
    bool        do_list_users = false;
    bool        do_mint       = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mint-enrollment-token" && i + 1 < argc) {
            do_mint    = true;
            mint_email = argv[++i];
        } else if (arg == "--ttl" && i + 1 < argc) {
            try {
                mint_ttl_secs = std::stoll(argv[++i]);
            } catch (...) {
                std::cerr << "ERROR: --ttl must be an integer number of seconds\n";
                return 1;
            }
        } else if (arg == "--list-users") {
            do_list_users = true;
        }
    }

    // --------------------------------------------------------------------------
    // Phase 4.E: Read TF_MASTER_KEY from environment.
    //
    // Policy (per Q3=A decision):
    //   - If TF_MASTER_KEY is set: use it as the SQLCipher database key.
    //   - If TF_MASTER_KEY is unset AND the database file does NOT exist:
    //       warn and proceed without encryption (dev fallback).
    //   - If TF_MASTER_KEY is unset AND the database file EXISTS:
    //       hard-fail — refuse to operate on a possibly-encrypted file without a key.
    //
    // GUARDRAIL F-1: master key is from env var only, not derived from any
    // caller-visible input.
    // GUARDRAIL: do NOT log the key value.
    // --------------------------------------------------------------------------
    std::optional<std::string> master_key_hex = read_master_key();

    // --------------------------------------------------------------------------
    // Admin CLI: --mint-enrollment-token
    // --------------------------------------------------------------------------
    if (do_mint) {
        if (mint_email.empty()) {
            std::cerr << "ERROR: --mint-enrollment-token requires an email argument\n";
            return 1;
        }
        try {
            std::string db_path = env_or("TF_DB_PATH", "dev/terminalfinance.db");
            Database db = open_db_with_migrations(db_path, std::move(master_key_hex));
            // Zero and release master_key_hex — no longer needed after DB open.
            if (master_key_hex.has_value()) {
                sodium_memzero(master_key_hex->data(), master_key_hex->size());
                master_key_hex.reset();
            }

            auto tok = tf::auth::mint_enrollment_token(mint_email,
                std::chrono::seconds(mint_ttl_secs));
            tf::auth::persist_enrollment_token(db, tok, mint_email);

            // Print ONLY the raw token — no other text.
            std::cout << tok.raw_token << "\n";
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: " << ex.what() << "\n";
            return 1;
        }
    }

    // --------------------------------------------------------------------------
    // Admin CLI: --list-users
    // --------------------------------------------------------------------------
    if (do_list_users) {
        try {
            std::string db_path = env_or("TF_DB_PATH", "dev/terminalfinance.db");
            Database db = open_db_with_migrations(db_path, std::move(master_key_hex));
            // Zero and release master_key_hex — no longer needed after DB open.
            if (master_key_hex.has_value()) {
                sodium_memzero(master_key_hex->data(), master_key_hex->size());
                master_key_hex.reset();
            }
            print_users(db);
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "ERROR: " << ex.what() << "\n";
            return 1;
        }
    }

    // --------------------------------------------------------------------------
    // Normal server boot path.
    // --------------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Read config from env vars.  All have sensible dev defaults.
    //
    //   TF_SERVER_PORT      default 8443
    //   TF_SERVER_CERT_PATH default dev/cert.pem
    //   TF_SERVER_KEY_PATH  default dev/key.pem
    //   TF_SERVER_BIND_ADDR default 127.0.0.1
    //   TF_MASTER_KEY       (no default — see encryption policy above)
    //
    // The dev/ directory is gitignored.  Run scripts/generate-dev-cert.sh to
    // populate it with mkcert-issued cert + key.
    // -----------------------------------------------------------------------
    ServerConfig cfg;
    cfg.port      = env_port("TF_SERVER_PORT", 8443);
    cfg.cert_path = env_or("TF_SERVER_CERT_PATH", "dev/cert.pem");
    cfg.key_path  = env_or("TF_SERVER_KEY_PATH",  "dev/key.pem");
    cfg.bind_addr = env_or("TF_SERVER_BIND_ADDR", "127.0.0.1");

    std::string db_path = env_or("TF_DB_PATH", "dev/terminalfinance.db");

    // Enforce master key policy for file databases (not applicable to :memory:).
    //
    // Detection logic:
    //   - If TF_MASTER_KEY is set: always use the key.
    //   - If TF_MASTER_KEY is unset AND file does NOT exist: create unencrypted
    //     (dev fallback; warn the operator).
    //   - If TF_MASTER_KEY is unset AND file EXISTS AND has the SQLite magic
    //     header ("SQLite format 3\x00"): it is a valid plain SQLite file;
    //     open without a key (warn; this handles dev/test scenarios where
    //     the server was previously run without encryption).
    //   - If TF_MASTER_KEY is unset AND file EXISTS AND does NOT have the
    //     SQLite magic: it may be an SQLCipher-encrypted file; hard-fail to
    //     prevent data corruption or silent key mismatch.
    if (!master_key_hex.has_value()) {
        bool db_exists = std::filesystem::exists(db_path);
        if (db_exists) {
            // Peek at the first 16 bytes: SQLite magic = "SQLite format 3\x00".
            static const char kSqliteMagic[] = "SQLite format 3";
            bool is_plain_sqlite = false;
            {
                std::ifstream f(db_path, std::ios::binary);
                if (f.is_open()) {
                    char buf[16] = {};
                    f.read(buf, 15);
                    is_plain_sqlite = (f.gcount() == 15) &&
                        (std::string(buf, 15) == kSqliteMagic);
                }
            }
            if (!is_plain_sqlite) {
                std::cerr << "ERROR: TF_MASTER_KEY is not set but database file '"
                          << db_path
                          << "' already exists and does not appear to be a plain SQLite file.\n"
                          << "  It may be an SQLCipher-encrypted database.\n"
                          << "  Set TF_MASTER_KEY to a 64-hex-char (32-byte) key, "
                             "or remove the database file to start fresh.\n";
                return 1;
            }
            // Plain SQLite file exists without a key — proceed with warning.
            std::cerr << "WARNING: TF_MASTER_KEY is not set — "
                         "opening existing unencrypted database at rest.\n"
                         "  This is acceptable for local development only.\n"
                         "  Set TF_MASTER_KEY to encrypt the database.\n";
        } else {
            // File does not exist — will be created unencrypted (dev fallback).
            std::cerr << "WARNING: TF_MASTER_KEY is not set — "
                         "database will be unencrypted at rest.\n"
                         "  This is acceptable for local development only.\n";
        }
    }

    // One-line status line for operators (GUARDRAIL: do not log key value).
    std::cout << "Database opened with master key: "
              << (master_key_hex.has_value() ? "yes" : "no") << "\n";

    std::cout << "TerminalFinance server v0.2\n"
              << "  bind_addr : " << cfg.bind_addr << "\n"
              << "  port      : " << cfg.port      << "\n"
              << "  cert_path : " << cfg.cert_path << "\n"
              << "  key_path  : " << cfg.key_path  << "\n"
              << "  db_path   : " << db_path        << "\n\n";

    // Open DB and apply migrations.
    // Use unique_ptr to allow deferred initialization while keeping RAII.
    std::unique_ptr<Database> db_ptr;
    try {
        db_ptr = std::make_unique<Database>(open_db_with_migrations(db_path, std::move(master_key_hex)));
        // Zero and release master_key_hex — no longer needed after DB open.
        if (master_key_hex.has_value()) {
            sodium_memzero(master_key_hex->data(), master_key_hex->size());
            master_key_hex.reset();
        }
        std::cout << "Database migrations applied.\n\n";
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: schema migration failed: " << ex.what() << "\n";
        return 1;
    }
    Database& db = *db_ptr;

    // Audit log: Phase 4 BLAKE2b-chained writer (replaces Phase 3 stub).
    tf::audit::SqlAuditLog audit_log(db);

    // PlaidTokenBroker (Phase 4.C): encrypts/decrypts Plaid access tokens.
    // Master key is sourced from TF_MASTER_KEY (64 hex chars = 32 bytes).
    // ASSUMPTION: 4.E (SQLCipher) has not shipped yet, so the broker reads
    // TF_MASTER_KEY independently.  Once 4.E ships, main.cpp should source
    // the master key once and pass it to both SQLCipher and PlaidTokenBroker
    // via the constructor that accepts a std::span<const std::byte, 32>.
    //
    // F-1 GUARDRAIL: the master key is read from env, NOT from any HTTP input.
    // The broker is stack-allocated here so it is destroyed (and key material
    // zeroed) before the process exits.
    std::unique_ptr<tf::plaid::PlaidTokenBroker> plaid_broker_ptr;
    {
        const char* tf_master_key = std::getenv("TF_MASTER_KEY");
        if (tf_master_key && tf_master_key[0] != '\0') {
            try {
                plaid_broker_ptr = std::make_unique<tf::plaid::PlaidTokenBroker>(db);
                std::cout << "PlaidTokenBroker initialized (TF_MASTER_KEY present).\n";
            } catch (const std::exception& ex) {
                std::cerr << "WARNING: PlaidTokenBroker init failed: " << ex.what()
                          << "\n  Plaid token storage will be unavailable.\n";
            }
        } else {
            std::cout << "WARNING: TF_MASTER_KEY not set — PlaidTokenBroker disabled. "
                      << "Set TF_MASTER_KEY=<64 hex chars> to enable Plaid token storage.\n";
        }
    }

    // -----------------------------------------------------------------------
    // Phase 4.D: PlaidApiClient + PlaidSyncScheduler.
    //
    // Only started if PLAID_CLIENT_ID and PLAID_SECRET are both set.
    // PlaidTokenBroker must also be available (TF_MASTER_KEY present).
    //
    // F-1 GUARDRAIL: Plaid creds are read from env vars inside PlaidApiClient
    //   constructor.  They are NEVER logged or stored outside that object.
    // -----------------------------------------------------------------------
    CurlHttpClient curl_http_client;
    std::unique_ptr<tf::plaid::PlaidApiClient>      plaid_api_ptr;
    std::unique_ptr<tf::plaid::PlaidSyncScheduler>  plaid_scheduler_ptr;

    {
        const char* plaid_cid = std::getenv("PLAID_CLIENT_ID");
        const char* plaid_sec = std::getenv("PLAID_SECRET");
        bool creds_present = (plaid_cid && plaid_cid[0] != '\0') &&
                             (plaid_sec && plaid_sec[0] != '\0');

        if (!creds_present) {
            std::cerr << "PlaidSyncScheduler: not started (PLAID_CLIENT_ID unset)\n";
        } else if (!plaid_broker_ptr) {
            std::cerr << "PlaidSyncScheduler: not started (PlaidTokenBroker unavailable)\n";
        } else {
            try {
                plaid_api_ptr = std::make_unique<tf::plaid::PlaidApiClient>(curl_http_client);
                plaid_scheduler_ptr = std::make_unique<tf::plaid::PlaidSyncScheduler>(
                    db, *plaid_broker_ptr, *plaid_api_ptr, audit_log);
            } catch (const std::exception& ex) {
                std::cerr << "WARNING: PlaidSyncScheduler init failed: " << ex.what()
                          << "\n  Plaid sync will be unavailable.\n";
                plaid_api_ptr.reset();
                plaid_scheduler_ptr.reset();
            }
        }
    }

    Server server(cfg);
    server.register_routes();

    // Wire auth routes.
    tf::auth::register_auth_handlers(server.raw_server(), db, audit_log);

    // Wire data routes (Phase 4.B).
    tf::data::register_entities_handlers(server.raw_server(), db, audit_log);
    tf::data::register_accounts_handlers(server.raw_server(), db, audit_log);
    tf::data::register_transactions_handlers(server.raw_server(), db, audit_log);
    tf::data::register_categories_handlers(server.raw_server(), db, audit_log);
    tf::data::register_budgets_handlers(server.raw_server(), db, audit_log);

    // TODO(v0.4-server): wire GET /sync-status endpoint.
    //
    //   The TUI's Drill_SyncStatus view (Task v0.3-3) already calls
    //   BackendClient::get_sync_status() which posts to GET /sync-status.
    //   Today the route does not exist server-side; the client treats
    //   the 404 as a feature flag ("endpoint not yet available; showing
    //   local cache") and falls back to the DataStore-derived view that
    //   the dashboard widget already renders.  Re-auth ([R] key) is
    //   gated behind a real response from this endpoint.
    //
    //   v0.4 work:
    //     1. Add a Phase-4-style data handler module (e.g. tf::data::
    //        register_sync_status_handler) that joins:
    //          - server-side accounts.institution (already present)
    //          - audit_log entries with action="sync_*" (newest per
    //            plaid_item_id) for last_success/last_attempt/error_code
    //        and emits the JSON contract documented in
    //        src/services/BackendClient.h (struct SyncStatusItem +
    //        get_sync_status doc).
    //     2. Register the handler here, session-gated on Bearer auth.
    //     3. Delete this TODO + the 404 fallback branch in
    //        BackendClient::get_sync_status() once the endpoint ships.
    //
    // Phase 5: GET /supplier-map — session-gated canonical supplier rules.
    // Reads data/supplier_map.json relative to CWD; TF_SUPPLIER_MAP_PATH
    // overrides for tests + alternate deployments.
    {
        std::string supplier_path = env_or("TF_SUPPLIER_MAP_PATH",
                                           "data/supplier_map.json");
        tf::discovery::register_supplier_map_handler(
            server.raw_server(), db, supplier_path);
    }

    // Start Plaid sync scheduler after all routes are registered.
    if (plaid_scheduler_ptr) {
        plaid_scheduler_ptr->start();
    }

    // Install SIGINT/SIGTERM handler so Ctrl-C exits cleanly.
    g_server = &server;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Listening on https://" << cfg.bind_addr
              << ":" << cfg.port << "  (Ctrl-C to exit)\n";

    if (!server.start()) {
        std::cerr << "ERROR: server failed to bind on "
                  << cfg.bind_addr << ":" << cfg.port
                  << " — check that the port is not already in use.\n";
        // Stop scheduler before returning.
        if (plaid_scheduler_ptr) {
            plaid_scheduler_ptr->stop();
        }
        return 1;
    }

    // start() blocks until stop() is called.
    // Stop the scheduler gracefully before the server exits.
    if (plaid_scheduler_ptr) {
        plaid_scheduler_ptr->stop();
    }

    std::cout << "Server stopped.\n";
    return 0;
}
