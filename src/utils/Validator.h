#pragma once

#include <string>
#include <vector>
#include <optional>

#include "../models/Transaction.h"
#include "../models/Account.h"
#include "../models/Category.h"
#include "../models/Entity.h"
#include "../utils/Logger.h"

#include <set>
#include <map>

struct ValidationResult {
    bool is_valid;
    std::string error_message;
    std::string anomaly_type;
};

class Validator {
public:
    static bool isValidDate(const std::string& date);
    static bool isValidAmount(double amount, const Account& account);
    static ValidationResult validateTransaction(const Transaction& tx, const Account& account);
    static ValidationResult validateAccount(const Account& acc, const std::vector<Entity>& entities);
    static ValidationResult validateCategory(const Category& cat, const std::vector<Category>& all_categories);
    static std::vector<ValidationResult> validateAllTransactions(
        const std::vector<Transaction>& transactions,
        const std::vector<Account>& accounts
    );
    static bool hasOrphanedCategories(
        const std::vector<Category>& categories,
        const std::vector<Transaction>& transactions
    );

private:
    static bool isValidAmount(double amount, AccountType account_type);
};