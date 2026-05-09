#include "PlaidService.h"
#include "../utils/Logger.h"

#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// env_to_hostname: shared by both platforms
// ---------------------------------------------------------------------------
namespace {

std::string env_to_hostname(PlaidEnvironment env) {
    switch (env) {
        case PlaidEnvironment::Development: return "development.plaid.com";
        case PlaidEnvironment::Production:  return "production.plaid.com";
        default:                            return "sandbox.plaid.com";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// RealPlaidService — backed by an injected IHttpClient (libcurl via
// CurlHttpClient in production; any IHttpClient in tests).
//
// All HTTP is done through IHttpClient::send(). No platform-specific HTTP
// code (winhttp, NSURLSession, …) appears here or anywhere in this class.
// ---------------------------------------------------------------------------
class RealPlaidService : public IPlaidService {
public:
    explicit RealPlaidService(std::shared_ptr<IHttpClient> http_client)
        : http_client_(std::move(http_client))
        , env_(PlaidEnvironment::Sandbox)
        , environment_(env_to_hostname(PlaidEnvironment::Sandbox))
        , timeout_(std::chrono::seconds{30})
    {}

    void set_timeout(std::chrono::seconds timeout) override {
        timeout_ = timeout;
    }

    bool initialize(const std::string& client_id,
                    const std::string& secret,
                    PlaidEnvironment env) override {
        client_id_ = client_id;
        secret_ = secret;
        env_ = env;
        environment_ = env_to_hostname(env_);
        initialized_ = !client_id.empty() && !secret.empty();
        if (!initialized_) {
            last_error_ = "Client ID and secret are required";
        }
        Logger::instance().info("PlaidService: Initialized for " + environment_);
        return initialized_;
    }

    std::string create_link_token() override {
        if (!initialized_) return "";

        const std::string body =
            R"({"client_name":"TerminalFinance","country_codes":["US"])"
            R"(,"language":"en","user":{"client_user_id":"terminal"})"
            R"(,"products":["transactions"]})";
        auto response = post_with_retry("/link/token/create", body, 3);

        try {
            auto j = json::parse(response);
            return j.value("link_token", "");
        } catch (...) {
            return "";
        }
    }

    std::optional<std::string> exchange_public_token(const std::string& public_token) override {
        if (!initialized_) return std::nullopt;

        const std::string body = "{\"public_token\":\"" + public_token + "\"}";
        auto response = post_with_retry("/item/public_token/exchange", body, 3);

        try {
            auto j = json::parse(response);
            auto token = j.value("access_token", "");
            return token.empty() ? std::nullopt : std::optional<std::string>(token);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<PlaidAccount> get_accounts(const std::string& access_token) override {
        std::vector<PlaidAccount> result;
        if (!initialized_ || access_token.empty()) return result;

        const std::string body = "{\"access_token\":\"" + access_token + "\"}";
        auto response = post_with_retry("/accounts/get", body, 3);

        try {
            auto j = json::parse(response);
            for (auto& acc : j["accounts"]) {
                PlaidAccount pa;
                pa.account_id = acc.value("account_id", "");
                pa.name       = acc.value("name", "");
                pa.type       = acc.value("type", "");
                pa.subtype    = acc.value("subtype", "");
                auto& balances = acc["balances"];
                pa.balance = balances.value("available",
                             balances.value("current", 0.0));
                result.push_back(pa);
            }
        } catch (const json::parse_error& e) {
            last_error_ = std::string("JSON parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        } catch (const std::exception& e) {
            last_error_ = std::string("Parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        }

        Logger::instance().info("PlaidService: Fetched " +
            std::to_string(result.size()) + " accounts");
        return result;
    }

    std::vector<PlaidTransaction> get_transactions(
        const std::string& access_token,
        const std::string& start_date,
        const std::string& end_date,
        int max_retries
    ) override {
        std::vector<PlaidTransaction> result;
        if (!initialized_ || access_token.empty()) return result;

        const std::string body =
            R"({"access_token":")" + access_token +
            R"(","start_date":")" + start_date +
            R"(","end_date":")" + end_date +
            R"(","options":{"page_size":500}})";
        auto response = post_with_retry("/transactions/get", body, max_retries);

        try {
            auto j = json::parse(response);
            for (auto& tx : j["transactions"]) {
                PlaidTransaction pt;
                pt.transaction_id = tx.value("transaction_id", "");
                pt.account_id     = tx.value("account_id", "");
                pt.amount         = tx.value("amount", 0.0);
                pt.date           = tx.value("date", "");
                pt.description    = tx.value("name", "");
                pt.pending        = tx.value("pending", false);
                auto& category    = tx["category"];
                if (!category.is_null() && category.is_array() && !category.empty()) {
                    pt.category = category[0].get<std::string>();
                }
                result.push_back(pt);
            }
        } catch (const json::parse_error& e) {
            last_error_ = std::string("JSON parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        } catch (const std::exception& e) {
            last_error_ = std::string("Parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        }

        Logger::instance().info("PlaidService: Fetched " +
            std::to_string(result.size()) + " transactions");
        return result;
    }

    bool remove_item(const std::string& access_token) override {
        if (!initialized_ || access_token.empty()) return false;

        const std::string body = "{\"access_token\":\"" + access_token + "\"}";
        auto response = post_with_retry("/item/remove", body, 3);

        try {
            auto j = json::parse(response);
            return j.value("removed", false);
        } catch (...) {
            return false;
        }
    }

    std::string get_last_error() const override { return last_error_; }
    bool is_stub() const override { return false; }

private:
    // Build the full HTTPS URL for a given Plaid API path.
    std::string build_url(const std::string& path) const {
        return "https://" + environment_ + path;
    }

    // POST to `path` with `body`, retrying up to `max_retries` times.
    // Returns the response body string, or "" on terminal failure.
    std::string post_with_retry(const std::string& path,
                                const std::string& body,
                                int max_retries) {
        int backoff_ms = 1000;

        for (int attempt = 0; attempt < max_retries; ++attempt) {
            auto result = post(path, body);
            if (result.has_value()) {
                return *result;
            }

            if (attempt + 1 < max_retries) {
                Logger::instance().info(
                    "PlaidService: Retrying in " + std::to_string(backoff_ms) +
                    "ms (attempt " + std::to_string(attempt + 2) +
                    "/" + std::to_string(max_retries) + ")");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                backoff_ms *= 2;
            }
        }

        last_error_ = "Failed after " + std::to_string(max_retries) + " attempts";
        Logger::instance().error("PlaidService: " + last_error_);
        return "";
    }

    // Single POST attempt. Returns std::nullopt on transport failure.
    std::optional<std::string> post(const std::string& path,
                                    const std::string& body) {
        HttpRequest req;
        req.method  = "POST";
        req.url     = build_url(path);
        req.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_);
        req.headers["Content-Type"]      = "application/json";
        req.headers["PLAID-CLIENT-ID"]   = client_id_;
        req.headers["PLAID-SECRET"]      = secret_;
        req.body = body;

        auto resp = http_client_->send(req);
        if (!resp.has_value()) {
            last_error_ = "HTTP transport failure for " + path;
            Logger::instance().error("PlaidService: " + last_error_);
            return std::nullopt;
        }

        if (resp->status_code < 200 || resp->status_code >= 300) {
            last_error_ = "HTTP " + std::to_string(resp->status_code) +
                          " for " + path + ": " + resp->body;
            Logger::instance().warning("PlaidService: " + last_error_);
            // Return the body anyway — Plaid embeds error details in it.
            return resp->body;
        }

        return resp->body;
    }

    std::shared_ptr<IHttpClient> http_client_;
    std::string client_id_;
    std::string secret_;
    mutable std::string last_error_;
    std::string environment_;
    PlaidEnvironment env_;
    bool initialized_ = false;
    std::chrono::seconds timeout_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::shared_ptr<IPlaidService> create_plaid_service(
    bool use_stub,
    std::shared_ptr<IHttpClient> http_client)
{
    if (use_stub || !http_client) {
        return std::make_shared<StubPlaidService>();
    }
    return std::make_shared<RealPlaidService>(std::move(http_client));
}
