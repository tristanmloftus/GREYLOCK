#include "PlaidService.h"
#include "../utils/Logger.h"
#include <thread>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <windef.h>

namespace {
    std::string widestring_to_string(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size_needed, NULL, NULL);
        return str;
    }

    std::wstring string_to_wide(const std::string& str) {
        if (str.empty()) return L"";
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstr(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
        return wstr;
    }

    std::string env_to_hostname(PlaidEnvironment env) {
        switch (env) {
            case PlaidEnvironment::Development: return "development.plaid.com";
            case PlaidEnvironment::Production:  return "production.plaid.com";
            default:                            return "sandbox.plaid.com";
        }
    }
}

class RealPlaidService : public IPlaidService {
public:
    RealPlaidService() : env_(PlaidEnvironment::Sandbox), timeout_(30) {
        environment_ = env_to_hostname(env_);
    }

    void set_timeout(std::chrono::seconds timeout) override {
        timeout_ = timeout;
    }

    bool initialize(const std::string& client_id, const std::string& secret, PlaidEnvironment env = PlaidEnvironment::Sandbox) override {
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

        std::string body = R"({"client_name":"TerminalFinance","country_codes":["US"],"language":"en","user":{"client_user_id":"terminal"},"products":["transactions"]})";
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

        std::string body = "{\"public_token\":\"" + public_token + "\"}";
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

        std::string body = "{\"access_token\":\"" + access_token + "\"}";
        auto response = post_with_retry("/accounts/get", body, 3);

        try {
            auto j = json::parse(response);
            auto& accounts = j["accounts"];
            for (auto& acc : accounts) {
                PlaidAccount pa;
                pa.account_id = acc.value("account_id", "");
                pa.name = acc.value("name", "");
                pa.type = acc.value("type", "");
                pa.subtype = acc.value("subtype", "");
                auto& balances = acc["balances"];
                pa.balance = balances.value("available", balances.value("current", 0.0));
                result.push_back(pa);
            }
        } catch (const json::parse_error& e) {
            last_error_ = std::string("JSON parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        } catch (const std::exception& e) {
            last_error_ = std::string("Parse error: ") + e.what();
            Logger::instance().error("PlaidService: " + last_error_);
        }

        Logger::instance().info("PlaidService: Fetched " + std::to_string(result.size()) + " accounts");
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

        std::string body = R"({"access_token":")" + access_token + R"(","start_date":")" +
                          start_date + R"(","end_date":")" + end_date + R"(","options":{"page_size":500}})";
        auto response = post_with_retry("/transactions/get", body, max_retries);

        try {
            auto j = json::parse(response);
            auto& transactions = j["transactions"];
            for (auto& tx : transactions) {
                PlaidTransaction pt;
                pt.transaction_id = tx.value("transaction_id", "");
                pt.account_id = tx.value("account_id", "");
                pt.amount = tx.value("amount", 0.0);
                pt.date = tx.value("date", "");
                pt.description = tx.value("name", "");
                pt.pending = tx.value("pending", false);
                auto& category = tx["category"];
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

        Logger::instance().info("PlaidService: Fetched " + std::to_string(result.size()) + " transactions");
        return result;
    }

    bool remove_item(const std::string& access_token) override {
        if (!initialized_ || access_token.empty()) return false;

        std::string body = "{\"access_token\":\"" + access_token + "\"}";
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
    std::string post_with_retry(const std::string& path, const std::string& body, int max_retries) {
        int attempts = 0;
        int backoff_ms = 1000;

        while (attempts < max_retries) {
            auto response = post(path, body);
            if (!response.empty()) {
                return response;
            }

            attempts++;
            if (attempts < max_retries) {
                Logger::instance().info("PlaidService: Retrying in " + std::to_string(backoff_ms) + "ms (attempt " + std::to_string(attempts + 1) + "/" + std::to_string(max_retries) + ")");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                backoff_ms *= 2;
            }
        }

        last_error_ = "Failed after " + std::to_string(max_retries) + " attempts";
        Logger::instance().error("PlaidService: " + last_error_);
        return "";
    }

    std::string post(const std::string& path, const std::string& body) {
        std::wstring wpath = string_to_wide(path);
        std::wstring whost = string_to_wide(environment_);
        std::wstring wbody = string_to_wide(body);

        HINTERNET session = WinHttpOpen(L"TerminalFinance/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, 0, 0);
        if (!session) return "";

        DWORD timeout_ms = static_cast<DWORD>(std::chrono::milliseconds(timeout_).count());
        WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

        HINTERNET connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { WinHttpCloseHandle(session); return ""; }

        HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return ""; }

        std::wstring wcontent_type = L"Content-Type: application/json";
        WinHttpAddRequestHeaders(request, wcontent_type.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        std::wstring wclient_id = L"PLAID-CLIENT-ID: " + string_to_wide(client_id_);
        WinHttpAddRequestHeaders(request, wclient_id.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        std::wstring wsecret = L"PLAID-SECRET: " + string_to_wide(secret_);
        WinHttpAddRequestHeaders(request, wsecret.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        BOOL send_result = WinHttpSendRequest(request, NULL, 0, (LPVOID)wbody.c_str(), (DWORD)wbody.size() * sizeof(wchar_t), (DWORD)wbody.size() * sizeof(wchar_t), 0);
        if (!send_result) {
            DWORD err = GetLastError();
            last_error_ = "WinHttpSendRequest failed: " + std::to_string(err);
            Logger::instance().error("PlaidService: " + last_error_);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return "";
        }

        WinHttpReceiveResponse(request, NULL);

        std::string raw_response;
        char buffer[4096];
        DWORD bytes_read = 0;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
            raw_response.append(buffer, bytes_read);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);

        return raw_response;
    }

    std::string client_id_;
    std::string secret_;
    std::string last_error_;
    std::string environment_;
    PlaidEnvironment env_;
    bool initialized_ = false;
    std::chrono::seconds timeout_;
};

#else

class RealPlaidService : public IPlaidService {
public:
    bool initialize(const std::string& client_id, const std::string& secret, PlaidEnvironment env = PlaidEnvironment::Sandbox) override { return false; }
    std::string create_link_token() override { return ""; }
    std::optional<std::string> exchange_public_token(const std::string& public_token) override { return std::nullopt; }
    std::vector<PlaidAccount> get_accounts(const std::string& access_token) override { return {}; }
    std::vector<PlaidTransaction> get_transactions(const std::string& access_token, const std::string& start_date, const std::string& end_date, int max_retries = 3) override { return {}; }
    bool remove_item(const std::string& access_token) override { return false; }
    std::string get_last_error() const override { return "Not implemented on this platform"; }
    bool is_stub() const override { return true; }
    void set_timeout(std::chrono::seconds timeout) override {}
};

#endif

std::shared_ptr<IPlaidService> create_plaid_service(bool use_stub) {
    if (use_stub) {
        return std::make_shared<StubPlaidService>();
    }
#ifdef _WIN32
    return std::make_shared<RealPlaidService>();
#else
    return std::make_shared<StubPlaidService>();
#endif
}