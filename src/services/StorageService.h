#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <ctime>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include "../models/Account.h"
#include "../models/Transaction.h"
#include "../models/Category.h"
#include "../models/Budget.h"
#include "../models/Entity.h"

using json = nlohmann::json;

class IStorageService : public std::enable_shared_from_this<IStorageService> {
public:
    virtual ~IStorageService() = default;

    virtual bool load(
        std::vector<Entity>& entities,
        std::vector<Account>& accounts,
        std::vector<Transaction>& transactions,
        std::vector<Category>& categories,
        std::vector<Budget>& budgets
    ) = 0;

    virtual bool save(
        const std::vector<Entity>& entities,
        const std::vector<Account>& accounts,
        const std::vector<Transaction>& transactions,
        const std::vector<Category>& categories,
        const std::vector<Budget>& budgets
    ) = 0;

    virtual std::string get_last_error() const = 0;
};

class JsonStorageService : public IStorageService {
public:
    explicit JsonStorageService(const std::string& filepath);

    bool load(
        std::vector<Entity>& entities,
        std::vector<Account>& accounts,
        std::vector<Transaction>& transactions,
        std::vector<Category>& categories,
        std::vector<Budget>& budgets
    ) override;

    bool save(
        const std::vector<Entity>& entities,
        const std::vector<Account>& accounts,
        const std::vector<Transaction>& transactions,
        const std::vector<Category>& categories,
        const std::vector<Budget>& budgets
    ) override;

    std::string get_last_error() const override { return last_error_; }

private:
    std::string filepath_;
    std::string last_error_;

    bool parse_entities(const json& j, std::vector<Entity>& entities);
    bool parse_accounts(const json& j, std::vector<Account>& accounts);
    bool parse_transactions(const json& j, std::vector<Transaction>& transactions);
    bool parse_categories(const json& j, std::vector<Category>& categories);
    bool parse_budgets(const json& j, std::vector<Budget>& budgets);
};