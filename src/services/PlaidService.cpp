// PlaidService.cpp — v0.2 server-mediated Plaid implementation.
//
// The TUI never holds a Plaid access_token.  All Plaid operations are
// proxied through the Greylock backend server, which holds the
// token encrypted via PlaidTokenBroker.
//
// F-1 / F-2: No plaintext token is handled in this file.  If you add a log
// statement, NEVER include token bytes.

#include "PlaidService.h"
#include "BackendClient.h"
#include "../utils/Logger.h"
#include "../utils/OpenBrowser.h"

#include <chrono>
#include <cstdlib>
#include <thread>

// ---------------------------------------------------------------------------
// ServerPlaidService — forwards Plaid operations to the Greylock server.
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
        last_link_url_.clear();

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

        // Construct the link URL pointing at OUR backend server's /link
        // endpoint. The backend serves an HTML page that loads
        // cdn.plaid.com's JS SDK inside the page. The TUI never opens
        // cdn.plaid.com directly; OpenBrowser's sanitizer rejects it.
        //
        // TF_BROWSER_URL overrides TF_BACKEND_URL for the URL the user's
        // browser will actually fetch.  When the TUI runs on a headless
        // server reachable over Tailscale, TF_BACKEND_URL is typically
        // https://localhost:8443 (loopback inside the box) but the user's
        // browser must hit the tailnet IP — set TF_BROWSER_URL to that.
        const char* browser_url_env = std::getenv("TF_BROWSER_URL");
        const char* backend_url_env = std::getenv("TF_BACKEND_URL");
        std::string base_url =
            (browser_url_env && browser_url_env[0] != '\0') ? std::string(browser_url_env)
          : (backend_url_env && backend_url_env[0] != '\0') ? std::string(backend_url_env)
          : std::string("https://localhost:8443");
        std::string link_url = base_url +
            "/link?account_id=" + account_id +
            "&link_token=" + link_token;
        last_link_url_ = link_url;

        // Try to open the browser locally.  On a Mac client this opens
        // Safari; on a headless Linux TUI host it will fail.  Either
        // way last_link_url_ is set so the TUI can display the URL for
        // the user to click/copy.
        const bool browser_opened = open_browser(link_url);
        if (!browser_opened) {
            Logger::instance().info(
                "PlaidService::initiate_link_flow: browser did not open locally; "
                "URL surfaced for user copy.");
        }

        // Poll the backend for sync completion.  Browser launching is
        // optional now — the user might copy the URL into Safari on a
        // different machine.  We still wait so the TUI can show
        // "linked" once the browser hits POST /accounts/:id/link-plaid.
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

    std::string last_link_url() const override { return last_link_url_; }

    // Non-blocking: mint URL, try local browser, return immediately.
    bool prepare_link_flow(const std::string& account_id) override {
        last_link_url_.clear();

        auto result = backend_->post(
            "/accounts/" + account_id + "/link/init", nlohmann::json::object());
        if (std::holds_alternative<BackendError>(result)) {
            last_error_ = "prepare_link_flow: " +
                std::get<BackendError>(result).message;
            return false;
        }

        auto& json = std::get<nlohmann::json>(result);
        auto link_token_it = json.find("link_token");
        if (link_token_it == json.end() || !link_token_it->is_string()) {
            last_error_ = "prepare_link_flow: no link_token in response";
            return false;
        }
        std::string link_token = link_token_it->get<std::string>();

        const char* browser_url_env = std::getenv("TF_BROWSER_URL");
        const char* backend_url_env = std::getenv("TF_BACKEND_URL");
        std::string base_url =
            (browser_url_env && browser_url_env[0] != '\0') ? std::string(browser_url_env)
          : (backend_url_env && backend_url_env[0] != '\0') ? std::string(backend_url_env)
          : std::string("https://localhost:8443");
        last_link_url_ = base_url +
            "/link?account_id=" + account_id +
            "&link_token=" + link_token;

        (void)open_browser(last_link_url_);  // best-effort; ignore result.
        return true;
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
    std::string last_link_url_;
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
