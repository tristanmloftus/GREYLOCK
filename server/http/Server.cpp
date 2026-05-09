// CPPHTTPLIB_OPENSSL_SUPPORT is defined via CMake target_compile_definitions so
// cpp-httplib's HTTPS path compiles.  It must appear before httplib.h is
// included for the first time in this translation unit.
#include "httplib.h"

#include "Server.h"
#include "HealthzHandler.h"

#include <atomic>
#include <stdexcept>
#include <string>

// --------------------------------------------------------------------------
// Impl: holds the httplib::SSLServer so its full type is only visible in
// this translation unit — callers only see an incomplete Impl*.
// --------------------------------------------------------------------------

struct Server::Impl {
    httplib::SSLServer ssl_server;
    // actual_port is written by start() after bind completes and before
    // listen_after_bind() blocks.  It is read by bound_port() from the
    // test fixture's main thread.  std::atomic<int> provides the necessary
    // happens-before guarantee without requiring a mutex.
    std::atomic<int> actual_port{-1};

    Impl(const std::string& cert_path, const std::string& key_path)
        : ssl_server(cert_path.c_str(), key_path.c_str())
    {}
};

// --------------------------------------------------------------------------
// Server implementation
// --------------------------------------------------------------------------

Server::Server(const ServerConfig& cfg)
    : cfg_(cfg)
{
    impl_ = new Impl(cfg_.cert_path, cfg_.key_path);

    if (!impl_->ssl_server.is_valid()) {
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error(
            "Server: SSLServer is not valid — check cert_path and key_path. "
            "cert_path=" + cfg_.cert_path + " key_path=" + cfg_.key_path);
    }
}

Server::~Server() {
    if (impl_) {
        impl_->ssl_server.stop();
        delete impl_;
        impl_ = nullptr;
    }
}

void Server::register_routes() {
    register_healthz_handler(impl_->ssl_server);
}

// start() uses cpp-httplib's two-phase bind API:
//
//   cfg_.port == 0 (ephemeral):
//     bind_to_any_port() asks the OS for a free port and returns the actual
//     port number (> 0) or a negative value on failure.
//
//   cfg_.port > 0 (explicit):
//     bind_to_port() binds the specified port and returns true on success.
//
//   After a successful bind, listen_after_bind() enters the accept loop.
//   It blocks until stop() is called from another thread.
//
// The actual port is stored in impl_->actual_port (atomic) between the bind
// and listen steps, so bound_port() is safe to call from any thread once
// start() has begun blocking in listen_after_bind().
bool Server::start() {
    if (cfg_.port == 0) {
        // Ephemeral port: ask the OS.  bind_to_any_port() returns the actual
        // port (> 0) or <= 0 on failure.
        int bound = impl_->ssl_server.bind_to_any_port(cfg_.bind_addr);
        if (bound <= 0) {
            return false;
        }
        impl_->actual_port.store(bound, std::memory_order_release);
    } else {
        // Explicit port.
        bool ok = impl_->ssl_server.bind_to_port(cfg_.bind_addr,
                                                  static_cast<int>(cfg_.port));
        if (!ok) {
            return false;
        }
        impl_->actual_port.store(static_cast<int>(cfg_.port),
                                 std::memory_order_release);
    }

    impl_->ssl_server.listen_after_bind();
    return true;
}

void Server::stop() {
    impl_->ssl_server.stop();
}

int Server::bound_port() const {
    return impl_->actual_port.load(std::memory_order_acquire);
}

httplib::SSLServer& Server::raw_server() {
    return impl_->ssl_server;
}
