#pragma once

#include <string>
#include <thread>
#include <functional>
#include <cstdint>

// Forward-declare httplib types so this header does not pull in httplib.h.
// Downstream translation units that call into httplib must include httplib.h
// themselves after including this header.
namespace httplib { class SSLServer; }

// Config that controls how TerminalFinanceServer binds and presents itself.
struct ServerConfig {
    std::string bind_addr{"127.0.0.1"};  // TF_SERVER_BIND_ADDR
    uint16_t    port{8443};               // TF_SERVER_PORT
    std::string cert_path;               // TF_SERVER_CERT_PATH
    std::string key_path;                // TF_SERVER_KEY_PATH
};

// Thin RAII wrapper around httplib::SSLServer.
//
// Lifecycle:
//   1. Construct with a ServerConfig.  The constructor creates the
//      SSLServer (loads cert + key, checks the SSL_CTX).  It does NOT bind
//      yet.
//   2. register_routes() adds all handlers (healthz, etc.).  Called once by
//      main() / the test fixture before start().
//   3. start() calls httplib::SSLServer::listen(), which binds + serves.
//      Blocks until stop() is called or an error occurs.  Intended to run
//      on a dedicated thread.
//   4. stop() calls httplib::SSLServer::stop().  Safe to call from any thread.
//      After stop() returns, the thread running start() will exit.
//
// Thread safety:
//   register_routes() must be called before start().  stop() may be called
//   concurrently with start() (from another thread) — that is the primary
//   use-case in the test fixture TearDown().
//
// Error handling:
//   - If cert_path or key_path do not exist / are not readable, the
//     httplib::SSLServer constructor signals failure (is_valid() returns false)
//     and Server's constructor throws std::runtime_error.
//   - If listen() fails (port already in use, etc.), start() returns false.
//
// Phase scope: v0.2 ships /healthz only.  Additional routes are registered by
// calling register_routes() on additional handler objects before start().  No
// auth, no DB, no Plaid in Phase 2.
class Server {
public:
    explicit Server(const ServerConfig& cfg);
    ~Server();

    // Non-copyable, non-movable.  The SSLServer owns sockets and callbacks.
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Register all built-in routes (healthz).  Must be called before start().
    void register_routes();

    // Bind and serve.  Blocks until stop() is called.  Returns false if
    // listen() could not bind.  Intended to run on a dedicated thread.
    bool start();

    // Signal the server to stop.  Returns immediately; actual stop is
    // asynchronous with respect to the thread running start().
    void stop();

    // Returns the actual port the server bound to.  Only valid after
    // start() has begun accepting connections.
    int bound_port() const;

    // Returns the underlying httplib::SSLServer (for advanced callers /
    // tests that need to add routes after construction).  Lifetime is
    // tied to this Server instance.
    httplib::SSLServer& raw_server();

private:
    ServerConfig cfg_;
    // httplib::SSLServer owns OpenSSL ctx + socket.  Stored as unique_ptr so
    // the destructor is predictable and we do not pull httplib.h into callers.
    // Defined in Server.cpp where httplib.h is included.
    struct Impl;
    Impl* impl_{nullptr};
};
