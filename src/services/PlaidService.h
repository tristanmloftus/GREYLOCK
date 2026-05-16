#pragma once

// PlaidService.h — TUI-side Plaid integration.
//
// v0.2 design (Q1=A: server-mediated Plaid):
//   The TUI never holds a Plaid access_token.  All Plaid operations go through
//   the server.  The public IPlaidService interface is account_id-based;
//   the server looks up the encrypted token, decrypts it in a broker scope,
//   and makes the Plaid API call.
//
//   IPlaidService::get_transactions(account_id, start_date, end_date) calls
//   GET /accounts/{account_id}/transactions on the server and returns the
//   cached transactions.
//
//   IPlaidService::link_account(account_id, public_token) calls
//   POST /accounts/{account_id}/link-plaid on the server, which exchanges the
//   public_token for an access_token and stores it encrypted via PlaidTokenBroker.
//
// F-1 / F-2: The plaintext Plaid access_token NEVER appears in TUI code,
//   in any struct field, or in any log message.

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <memory>

#include "../models/Account.h"
#include "../models/Transaction.h"

struct PlaidTransaction {
    std::string transaction_id;
    std::string account_id;
    std::string date;
    double amount;
    std::string description;
    std::string category;
    bool pending;
};

struct PlaidAccount {
    std::string account_id;
    std::string name;
    std::string type;
    std::string subtype;
    double balance;
};

enum class PlaidEnvironment {
    Sandbox,
    Development,
    Production
};

// ---------------------------------------------------------------------------
// IPlaidService — v0.2 account_id-based interface.
//
// NO method accepts or returns a Plaid access_token.  Token management is
// the server's responsibility (PlaidTokenBroker).
// ---------------------------------------------------------------------------
class IPlaidService {
public:
    virtual ~IPlaidService() = default;

    // Exchange a Plaid Link public_token for an access_token on the server.
    // The server stores the token encrypted.  Returns true on success.
    virtual bool link_account(const std::string& account_id,
                              const std::string& public_token) = 0;

    // Initiate the full Plaid Link flow for account_id.
    // 1. POST /accounts/:id/link/init → gets link_token
    // 2. Open browser to Plaid Link page
    // 3. Poll is_plaid_linked (2s × up to 150 = 5 min timeout)
    // Returns true once the account is linked.
    virtual bool initiate_link_flow(const std::string& account_id) = 0;

    // Retrieve cached transactions for the given account from the server.
    // The server performs the Plaid API call using the stored encrypted token.
    virtual std::vector<PlaidTransaction> get_transactions(
        const std::string& account_id,
        const std::string& start_date,
        const std::string& end_date
    ) = 0;

    // Retrieve accounts for the given account_id from the server.
    virtual std::vector<PlaidAccount> get_accounts(
        const std::string& account_id
    ) = 0;

    // Unlink the given account (clear its stored Plaid token on the server).
    virtual bool unlink_account(const std::string& account_id) = 0;

    virtual std::string get_last_error() const = 0;
    virtual bool is_stub() const = 0;
    virtual void set_timeout(std::chrono::seconds timeout) = 0;
};

// ---------------------------------------------------------------------------
// StubPlaidService — used in tests and when no server is available.
//
// All operations succeed trivially; no network calls, no token handling.
// ---------------------------------------------------------------------------
class StubPlaidService : public IPlaidService {
public:
    bool link_account(const std::string& /*account_id*/,
                      const std::string& /*public_token*/) override {
        return true;
    }

    bool initiate_link_flow(const std::string& /*account_id*/) override {
        return true;
    }

    std::vector<PlaidTransaction> get_transactions(
        const std::string& /*account_id*/,
        const std::string& /*start_date*/,
        const std::string& /*end_date*/
    ) override {
        return {};
    }

    std::vector<PlaidAccount> get_accounts(
        const std::string& /*account_id*/
    ) override {
        return {};
    }

    bool unlink_account(const std::string& /*account_id*/) override {
        return true;
    }

    std::string get_last_error() const override { return last_error_; }
    bool is_stub() const override { return true; }
    void set_timeout(std::chrono::seconds timeout) override { timeout_ = timeout; }

private:
    std::string last_error_;
    std::chrono::seconds timeout_{30};
};

// Factory: returns StubPlaidService when use_stub=true or backend is null.
// Production path (use_stub=false) returns a server-mediated implementation
// that calls the TerminalFinance backend via BackendClient.
std::shared_ptr<IPlaidService> create_plaid_service(bool use_stub = false);
std::shared_ptr<IPlaidService> create_plaid_service(
    std::shared_ptr<class BackendClient> backend);
