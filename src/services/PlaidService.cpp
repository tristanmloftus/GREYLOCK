// PlaidService.cpp — v0.2 server-mediated Plaid implementation.
//
// The TUI never holds a Plaid access_token.  All Plaid operations are
// proxied through the TerminalFinance backend server, which holds the
// token encrypted via PlaidTokenBroker.
//
// F-1 / F-2: No plaintext token is handled in this file.  If you add a log
// statement, NEVER include token bytes.

#include "PlaidService.h"
#include "../utils/Logger.h"

// ---------------------------------------------------------------------------
// ServerPlaidService — forwards Plaid operations to the TerminalFinance server.
//
// In v0.2, the server endpoints are:
//   POST /accounts/{id}/link-plaid         — exchange public_token
//   GET  /accounts/{id}/transactions       — get cached transactions
//   GET  /accounts/{id}/plaid-accounts     — get Plaid account info
//   DELETE /accounts/{id}/plaid-link       — unlink / clear token
//
// These endpoints are defined in 4.D (PlaidHandlers); 4.C ships this thin
// client wrapper that 4.D can flesh out with the actual server endpoints.
// ---------------------------------------------------------------------------
class ServerPlaidService : public IPlaidService {
public:
    ServerPlaidService() = default;

    bool link_account(const std::string& account_id,
                      const std::string& /*public_token*/) override {
        // TODO (4.D): POST /accounts/{account_id}/link-plaid via BackendClient.
        // The server exchanges public_token for access_token and stores it
        // encrypted via PlaidTokenBroker::store_token().
        Logger::instance().info(
            "PlaidService: link_account called for account " + account_id +
            " (server endpoint wiring deferred to 4.D)");
        last_error_ = "link_account: server endpoint not yet wired (4.D)";
        return false;
    }

    std::vector<PlaidTransaction> get_transactions(
        const std::string& account_id,
        const std::string& start_date,
        const std::string& end_date
    ) override {
        // TODO (4.D): GET /accounts/{account_id}/transactions via BackendClient.
        Logger::instance().info(
            "PlaidService: get_transactions called for account " + account_id +
            " [" + start_date + " – " + end_date + "]"
            " (server endpoint wiring deferred to 4.D)");
        last_error_ = "get_transactions: server endpoint not yet wired (4.D)";
        return {};
    }

    std::vector<PlaidAccount> get_accounts(
        const std::string& account_id
    ) override {
        // TODO (4.D): GET /accounts/{account_id}/plaid-accounts via BackendClient.
        Logger::instance().info(
            "PlaidService: get_accounts called for account " + account_id +
            " (server endpoint wiring deferred to 4.D)");
        last_error_ = "get_accounts: server endpoint not yet wired (4.D)";
        return {};
    }

    bool unlink_account(const std::string& account_id) override {
        // TODO (4.D): DELETE /accounts/{account_id}/plaid-link via BackendClient.
        Logger::instance().info(
            "PlaidService: unlink_account called for account " + account_id +
            " (server endpoint wiring deferred to 4.D)");
        last_error_ = "unlink_account: server endpoint not yet wired (4.D)";
        return false;
    }

    std::string get_last_error() const override { return last_error_; }
    bool is_stub() const override { return false; }
    void set_timeout(std::chrono::seconds timeout) override { timeout_ = timeout; }

private:
    mutable std::string last_error_;
    std::chrono::seconds timeout_{30};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::shared_ptr<IPlaidService> create_plaid_service(bool use_stub) {
    if (use_stub) {
        return std::make_shared<StubPlaidService>();
    }
    return std::make_shared<ServerPlaidService>();
}
