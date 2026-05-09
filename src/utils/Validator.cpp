#include "Validator.h"

bool Validator::isValidDate(const std::string& date) {
    if (date.length() != 10) return false;
    if (date[4] != '-' || date[7] != '-') return false;

    try {
        int year = std::stoi(date.substr(0, 4));
        int month = std::stoi(date.substr(5, 2));
        int day = std::stoi(date.substr(8, 2));

        if (year < 1900 || year > 2100) return false;
        if (month < 1 || month > 12) return false;
        if (day < 1 || day > 31) return false;

        static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (day > days_in_month[month - 1]) return false;

        return true;
    } catch (...) {
        return false;
    }
}

bool Validator::isValidAmount(double amount, AccountType account_type) {
    switch (account_type) {
        case AccountType::CreditCard:
            return true;
        case AccountType::Investment:
            return true;
        case AccountType::Checking:
        case AccountType::Savings:
        case AccountType::Cash:
            return amount >= 0;
        default:
            return true;
    }
}

bool Validator::isValidAmount(double amount, const Account& account) {
    return isValidAmount(amount, account.type);
}

ValidationResult Validator::validateTransaction(const Transaction& tx, const Account& account) {
    if (tx.id.empty()) {
        return {false, "Transaction ID is empty", "missing_id"};
    }

    if (!isValidDate(tx.date)) {
        return {false, "Invalid date format: " + tx.date, "invalid_date"};
    }

    if (!isValidAmount(tx.amount, account)) {
        return {false, "Invalid amount for account type: negative balance in non-credit account", "invalid_amount"};
    }

    return {true, "", ""};
}

ValidationResult Validator::validateAccount(const Account& acc, const std::vector<Entity>& entities) {
    if (acc.id.empty()) {
        return {false, "Account ID is empty", "missing_id"};
    }

    if (!acc.entity_id.empty()) {
        bool found = false;
        for (const auto& e : entities) {
            if (e.id == acc.entity_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return {false, "Account references non-existent entity: " + acc.entity_id, "orphan_entity"};
        }
    }

    return {true, "", ""};
}

ValidationResult Validator::validateCategory(const Category& cat, const std::vector<Category>& all_categories) {
    if (cat.id.empty()) {
        return {false, "Category ID is empty", "missing_id"};
    }

    if (!cat.parent_id.empty()) {
        bool found = false;
        for (const auto& c : all_categories) {
            if (c.id == cat.parent_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return {false, "Category references non-existent parent: " + cat.parent_id, "orphan_parent"};
        }
    }

    return {true, "", ""};
}

std::vector<ValidationResult> Validator::validateAllTransactions(
    const std::vector<Transaction>& transactions,
    const std::vector<Account>& accounts
) {
    std::vector<ValidationResult> results;
    results.reserve(transactions.size());

    std::map<std::string, Account> account_map;
    for (const auto& acc : accounts) {
        account_map[acc.id] = acc;
    }

    for (const auto& tx : transactions) {
        auto it = account_map.find(tx.account_id);
        if (it != account_map.end()) {
            results.push_back(validateTransaction(tx, it->second));
        } else {
            results.push_back({false, "Transaction references non-existent account: " + tx.account_id, "orphan_account"});
        }
    }

    return results;
}

bool Validator::hasOrphanedCategories(
    const std::vector<Category>& categories,
    const std::vector<Transaction>& transactions
) {
    std::set<std::string> used_categories;
    for (const auto& tx : transactions) {
        if (!tx.category_id.empty()) {
            used_categories.insert(tx.category_id);
        }
    }

    for (const auto& cat : categories) {
        if (!cat.is_system && used_categories.find(cat.id) == used_categories.end()) {
            return true;
        }
    }

    return false;
}