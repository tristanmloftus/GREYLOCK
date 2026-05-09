#include "http/Server.h"

#include <cstdlib>
#include <cstdint>
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

    std::cout << "TerminalFinance server v0.2\n"
              << "  bind_addr : " << cfg.bind_addr << "\n"
              << "  port      : " << cfg.port      << "\n"
              << "  cert_path : " << cfg.cert_path << "\n"
              << "  key_path  : " << cfg.key_path  << "\n\n";

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
