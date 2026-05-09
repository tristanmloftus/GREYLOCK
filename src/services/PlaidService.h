#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <memory>

#include "../models/Account.h"
#include "../models/Transaction.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum class PlaidEnvironment {
    Sandbox,
    Development,
    Production
};

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

class IPlaidService {
public:
    virtual ~IPlaidService() = default;

    virtual bool initialize(const std::string& client_id, const std::string& secret, PlaidEnvironment env = PlaidEnvironment::Sandbox) = 0;
    virtual std::string create_link_token() = 0;
    virtual std::optional<std::string> exchange_public_token(const std::string& public_token) = 0;
    virtual std::vector<PlaidAccount> get_accounts(const std::string& access_token) = 0;
    virtual std::vector<PlaidTransaction> get_transactions(
        const std::string& access_token,
        const std::string& start_date,
        const std::string& end_date,
        int max_retries = 3
    ) = 0;
    virtual bool remove_item(const std::string& access_token) = 0;
    virtual std::string get_last_error() const = 0;
    virtual bool is_stub() const = 0;
    virtual void set_timeout(std::chrono::seconds timeout) = 0;
};

class StubPlaidService : public IPlaidService {
public:
    bool initialize(const std::string& client_id, const std::string& secret, PlaidEnvironment env = PlaidEnvironment::Sandbox) override {
        client_id_ = client_id;
        secret_ = secret;
        env_ = env;
        initialized_ = true;
        return true;
    }

    std::string create_link_token() override {
        return "link-sandbox-xxxxx";
    }

    std::optional<std::string> exchange_public_token(const std::string& public_token) override {
        return "access-sandbox-xxxxx";
    }

    std::vector<PlaidAccount> get_accounts(const std::string& access_token) override {
        return {};
    }

    std::vector<PlaidTransaction> get_transactions(
        const std::string& access_token,
        const std::string& start_date,
        const std::string& end_date,
        int max_retries = 3
    ) override {
        return {};
    }

    bool remove_item(const std::string& access_token) override {
        return true;
    }

    std::string get_last_error() const override { return last_error_; }
    bool is_stub() const override { return true; }
    void set_timeout(std::chrono::seconds timeout) override { timeout_ = timeout; }

private:
    std::string client_id_;
    std::string secret_;
    PlaidEnvironment env_ = PlaidEnvironment::Sandbox;
    std::string last_error_;
    bool initialized_ = false;
    std::chrono::seconds timeout_{30};
};

std::shared_ptr<IPlaidService> create_plaid_service(bool use_stub = false);