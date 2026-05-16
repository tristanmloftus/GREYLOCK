// PlaidService.cpp — v0.2 server-mediated Plaid implementation.
//
// The TUI never holds a Plaid access_token.  All Plaid operations are
// proxied through the TerminalFinance backend server, which holds the
// token encrypted via PlaidTokenBroker.
//
// F-1 / F-2: No plaintext token is handled in this file.  If you add a log
// statement, NEVER include token bytes.

#include "PlaidService.h"
#include "BackendClient.h"
#include "../utils/Logger.h"
#include "../utils/OpenBrowser.h"

#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// ServerPlaidService — forwards Plaid operations to the TerminalFinance server.
// ---------------------------------------------------------------------------
class ServerPlaidService : public IPlaidService {
public:
    explicit ServerPlaidService(std::shared_ptr<BackendClient> backend)
        : backend_(std::move(backend)) {}

    bool link_account(const std::string& account_id,
                      const std::string& public_token) override {
        nlohmann::json body = {{"public_token", public_token}};
        auto result = backend_->post(
            "/accounts/" + account_id + "/link-plaid", body);
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "link_account: " +
                std::get<BackendError>(result).message;
            return false;
        }
        return true;
    }

    bool initiate_link_flow(const std::string& account_id) override {
        auto result = backend_->post(
            "/accounts/" + account_id + "/link/init", nlohmann::json::object());
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "initiate_link_flow: " +
                std::get<BackendError>(result).message;
            return false;
        }

        auto& json = std::get<nlohmann::json>(result);
        auto link_token_it = json.find("link_token");
        if (link_token_it == json.end() || !link_token_it->is_string()) {
            last_error_ = "initiate_link_flow: no link_token in response";
            return false;
        }
        std::string link_token = link_token_it->get<std::string>();

        std::string link_url =
            "https://cdn.plaid.com/link/v2/stable/link-initialize.html"
            "?token=" + link_token;

        if (!open_browser(link_url)) {
            last_error_ = "initiate_link_flow: failed to open browser";
            return false;
        }

        int max_polls = 150;
        for (int i = 0; i < max_polls; ++i) {
            auto sync_result = backend_->post(
                "/accounts/" + account_id + "/sync", nlohmann::json::object());
            if (!std::holds_alternative<BackendError>(sync_result)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        last_error_ = "initiate_link_flow: timeout waiting for link completion";
        return false;
    }

    std::vector<PlaidTransaction> get_transactions(
        const std::string& account_id,
        const std::string& start_date,
        const std::string& end_date
    ) override {
        auto result = backend_->get(
            "/accounts/" + account_id + "/transactions");
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "get_transactions: " +
                std::get<BackendError>(result).message;
            return {};
        }
        return {};
    }

    std::vector<PlaidAccount> get_accounts(
        const std::string& account_id
    ) override {
        auto result = backend_->get(
            "/accounts/" + account_id + "/plaid-accounts");
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "get_accounts: " +
                std::get<BackendError>(result).message;
            return {};
        }
        return {};
    }

    bool unlink_account(const std::string& account_id) override {
        auto result = backend_->post(
            "/accounts/" + account_id + "/unlink", nlohmann::json::object());
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "unlink_account: " +
                std::get<BackendError>(result).message;
            return false;
        }
        return true;
    }

    std::string get_last_error() const override { return last_error_; }
    bool is_stub() const override { return false; }
    void set_timeout(std::chrono::seconds timeout) override { timeout_ = timeout; }

private:
    mutable std::string last_error_;
    std::chrono::seconds timeout_{30};
    std::shared_ptr<BackendClient> backend_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::shared_ptr<IPlaidService> create_plaid_service(bool use_stub) {
    if (use_stub) {
        return std::make_shared<StubPlaidService>();
    }
    return std::make_shared<ServerPlaidService>(nullptr);
}

std::shared_ptr<IPlaidService> create_plaid_service(
    std::shared_ptr<BackendClient> backend)
{
    if (!backend) {
        return std::make_shared<StubPlaidService>();
    }
    return std::make_shared<ServerPlaidService>(std::move(backend));
}
