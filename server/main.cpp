#include "http/Server.h"
#include "db/Database.h"
#include "db/Migrations.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

int main() {
    // -----------------------------------------------------------------------
    // Read config from env vars.  All have sensible dev defaults.
    //
    //   TF_SERVER_PORT      default 8443
    //   TF_SERVER_CERT_PATH default dev/cert.pem
    //   TF_SERVER_KEY_PATH  default dev/key.pem
    //   TF_SERVER_BIND_ADDR default 127.0.0.1
    //
    // The dev/ directory is gitignored.  Run scripts/generate-dev-cert.sh to
    // populate it with mkcert-issued cert + key.
    // -----------------------------------------------------------------------
    ServerConfig cfg;
    cfg.port      = env_port("TF_SERVER_PORT", 8443);
    cfg.cert_path = env_or("TF_SERVER_CERT_PATH", "dev/cert.pem");
    cfg.key_path  = env_or("TF_SERVER_KEY_PATH",  "dev/key.pem");
    cfg.bind_addr = env_or("TF_SERVER_BIND_ADDR", "127.0.0.1");

    // -----------------------------------------------------------------------
    // Database path.
    //   TF_DB_PATH  default dev/terminalfinance.db
    // -----------------------------------------------------------------------
    std::string db_path = env_or("TF_DB_PATH", "dev/terminalfinance.db");

    std::cout << "TerminalFinance server v0.2\n"
              << "  bind_addr : " << cfg.bind_addr << "\n"
              << "  port      : " << cfg.port      << "\n"
              << "  cert_path : " << cfg.cert_path << "\n"
              << "  key_path  : " << cfg.key_path  << "\n"
              << "  db_path   : " << db_path        << "\n\n";

    // -----------------------------------------------------------------------
    // Open the database and apply schema migrations.
    // The dev/ directory is gitignored.  Create it if it does not exist so the
    // first run doesn't fail with "unable to open database file".
    // -----------------------------------------------------------------------
    {
        std::filesystem::path db_file_path(db_path);
        if (db_file_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(db_file_path.parent_path(), ec);
            if (ec) {
                std::cerr << "ERROR: could not create database directory '"
                          << db_file_path.parent_path().string()
                          << "': " << ec.message() << "\n";
                return 1;
            }
        }
    }

    Database db(db_path);

    Migrations migrations;
    migrations.register_migration({1, "M001_initial_schema", M001_initial_schema_up});

    try {
        migrations.apply_pending(db);
        int ver = migrations.current_version(db);
        std::cout << "Database schema at version " << ver << " (all migrations applied).\n\n";
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: schema migration failed: " << ex.what() << "\n";
        return 1;
    }

    Server server(cfg);
    server.register_routes();

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
        return 1;
    }

    // start() blocks until stop() is called.
    std::cout << "Server stopped.\n";
    return 0;
}
