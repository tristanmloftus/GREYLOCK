#include "DataStore.h"

#include <ctime>
#include <fstream>
#include <algorithm>
#include <regex>

namespace {
    bool is_valid_date(const std::string& date) {
        if (date.empty()) return true;
        std::regex date_pattern(R"(\d{4}-\d{2}-\d{2})");
        if (!std::regex_match(date, date_pattern)) return false;

        int year = std::stoi(date.substr(0, 4));
        int month = std::stoi(date.substr(5, 2));
        int day = std::stoi(date.substr(8, 2));

        if (month < 1 || month > 12) return false;
        if (day < 1 || day > 31) return false;
        if (year < 1900 || year > 2100) return false;
        return true;
    }
}

DataStore::DataStore(std::shared_ptr<IStorageService> storage)
    : storage_(std::move(storage))
{
    init_default_categories();
}

std::string DataStore::generate_id() {
    static int counter = 0;
    return "id_" + std::to_string(++counter) + "_" + std::to_string(time(nullptr));
}

void DataStore::set_storage(std::shared_ptr<IStorageService> storage) {
    storage_ = std::move(storage);
}

// Entity operations
Entity& DataStore::add_entity(Entity entity) {
    if (entity.id.empty()) {
        entity.id = generate_id();
    }
    entities.push_back(std::move(entity));
    return entities.back();
}

void DataStore::remove_entity(const std::string& entity_id) {
    entities.erase(
        std::remove_if(entities.begin(), entities.end(),
            [&entity_id](const Entity& e) { return e.id == entity_id; }),
        entities.end()
    );
}

std::optional<Entity*> DataStore::get_entity(const std::string& entity_id) {
    for (auto& e : entities) {
        if (e.id == entity_id) return &e;
    }
    return std::nullopt;
}

std::vector<Entity*> DataStore::get_all_entities() {
    std::vector<Entity*> result;
    for (auto& e : entities) {
        if (e.is_active) {
            result.push_back(&e);
        }
    }
    return result;
}

std::string DataStore::get_last_error() const {
    if (storage_) {
        return storage_->get_last_error();
    }
    return last_error_;
}

bool DataStore::load() {
    if (!storage_) {
        last_error_ = "No storage service configured";
        return false;
    }
    bool result = storage_->load(entities, accounts, transactions, categories, budgets);
    if (!result) {
        last_error_ = storage_->get_last_error();
    }
    return result;
}

bool DataStore::save() {
    if (!storage_) {
        last_error_ = "No storage service configured";
        return false;
    }
    bool result = storage_->save(entities, accounts, transactions, categories, budgets);
    if (!result) {
        last_error_ = storage_->get_last_error();
    }
    return result;
}

Account& DataStore::add_account(Account account) {
    if (account.id.empty()) {
        account.id = generate_id();
    }

    if (!account.entity_id.empty()) {
        bool entity_exists = false;
        for (const auto& e : entities) {
            if (e.id == account.entity_id) {
                entity_exists = true;
                break;
            }
        }
        if (!entity_exists) {
            last_error_ = "Cannot add account: entity_id '" + account.entity_id + "' does not exist";
            account.entity_id.clear();
        }
    }

    accounts.push_back(std::move(account));
    return accounts.back();
}

void DataStore::remove_account(const std::string& account_id) {
    accounts.erase(
        std::remove_if(accounts.begin(), accounts.end(),
            [&account_id](const Account& a) { return a.id == account_id; }),
        accounts.end()
    );
}

std::optional<Account*> DataStore::get_account(const std::string& account_id) {
    for (auto& acc : accounts) {
        if (acc.id == account_id) return &acc;
    }
    return std::nullopt;
}

std::vector<Account*> DataStore::get_accounts_by_type(AccountType type) {
    std::vector<Account*> result;
    for (auto& acc : accounts) {
        if (acc.type == type && acc.is_active) {
            result.push_back(&acc);
        }
    }
    return result;
}

std::vector<Account*> DataStore::get_accounts_for_entity(const std::string& entity_id) {
    std::vector<Account*> result;
    for (auto& acc : accounts) {
        if (acc.entity_id == entity_id && acc.is_active) {
            result.push_back(&acc);
        }
    }
    return result;
}

Transaction& DataStore::add_transaction(Transaction transaction) {
    if (transaction.id.empty()) {
        transaction.id = generate_id();
    }

    if (!is_valid_date(transaction.date)) {
        last_error_ = "Invalid date format: '" + transaction.date + "' (expected YYYY-MM-DD)";
    }

    bool account_exists = false;
    for (const auto& acc : accounts) {
        if (acc.id == transaction.account_id) {
            account_exists = true;
            break;
        }
    }
    if (!account_exists && !transaction.account_id.empty()) {
        last_error_ = "Warning: transaction references non-existent account";
    }

    transactions.push_back(std::move(transaction));
    return transactions.back();
}

void DataStore::remove_transaction(const std::string& transaction_id) {
    transactions.erase(
        std::remove_if(transactions.begin(), transactions.end(),
            [&transaction_id](const Transaction& t) { return t.id == transaction_id; }),
        transactions.end()
    );
}

std::optional<Transaction*> DataStore::get_transaction(const std::string& transaction_id) {
    for (auto& t : transactions) {
        if (t.id == transaction_id) return &t;
    }
    return std::nullopt;
}

std::vector<Transaction*> DataStore::get_transactions_for_account(const std::string& account_id) {
    std::vector<Transaction*> result;
    for (auto& t : transactions) {
        if (t.account_id == account_id) {
            result.push_back(&t);
        }
    }
    std::sort(result.begin(), result.end(),
        [](Transaction* a, Transaction* b) { return a->date > b->date; });
    return result;
}

std::vector<Transaction*> DataStore::get_transactions_for_entity(const std::string& entity_id) {
    std::vector<Transaction*> result;
    for (auto& acc : accounts) {
        if (acc.entity_id == entity_id) {
            for (auto& t : transactions) {
                if (t.account_id == acc.id) {
                    result.push_back(&t);
                }
            }
        }
    }
    std::sort(result.begin(), result.end(),
        [](Transaction* a, Transaction* b) { return a->date > b->date; });
    return result;
}

std::vector<Transaction*> DataStore::filter_transactions(const TransactionFilter& filter) {
    std::vector<Transaction*> result;
    for (auto& t : transactions) {
        bool matches = true;

        if (!filter.account_id.empty() && t.account_id != filter.account_id) matches = false;
        if (!filter.category_id.empty() && t.category_id != filter.category_id) matches = false;
        if (!filter.start_date.empty() && t.date < filter.start_date) matches = false;
        if (!filter.end_date.empty() && t.date > filter.end_date) matches = false;
        if (filter.min_amount != 0 && t.amount > filter.min_amount) matches = false;
        if (filter.max_amount != 0 && t.amount < filter.max_amount) matches = false;
        if (filter.show_pending_only && !t.pending) matches = false;
        if (filter.show_expenses_only && !t.is_expense()) matches = false;
        if (filter.show_income_only && !t.is_income()) matches = false;
        if (!filter.search_text.empty() &&
            t.description.find(filter.search_text) == std::string::npos) matches = false;

        if (matches) result.push_back(&t);
    }
    return result;
}

void DataStore::update_transaction_category(const std::string& transaction_id, const std::string& category_id) {
    for (auto& t : transactions) {
        if (t.id == transaction_id) {
            t.category_id = category_id;
            return;
        }
    }
}

void DataStore::init_default_categories() {
    categories = DEFAULT_CATEGORIES;
}

std::optional<Category*> DataStore::get_category(const std::string& category_id) {
    for (auto& c : categories) {
        if (c.id == category_id) return &c;
    }
    return std::nullopt;
}

std::vector<Category*> DataStore::get_expense_categories() {
    std::vector<Category*> result;
    for (auto& c : categories) {
        if (c.type == CategoryType::Expense && c.parent_id.empty()) {
            result.push_back(&c);
        }
    }
    return result;
}

std::vector<Category*> DataStore::get_income_categories() {
    std::vector<Category*> result;
    for (auto& c : categories) {
        if (c.type == CategoryType::Income && c.parent_id.empty()) {
            result.push_back(&c);
        }
    }
    return result;
}

Category& DataStore::add_category(Category category) {
    if (category.id.empty()) {
        category.id = generate_id();
    }
    category.is_system = false;
    categories.push_back(std::move(category));
    return categories.back();
}

Budget& DataStore::add_budget(Budget budget) {
    if (budget.id.empty()) {
        budget.id = generate_id();
    }
    budgets.push_back(std::move(budget));
    return budgets.back();
}

std::optional<Budget*> DataStore::get_budget(const std::string& category_id, const std::string& month) {
    for (auto& b : budgets) {
        if (b.category_id == category_id && b.month == month) return &b;
    }
    return std::nullopt;
}

std::vector<Budget*> DataStore::get_budgets_for_month(const std::string& month) {
    std::vector<Budget*> result;
    for (auto& b : budgets) {
        if (b.month == month) result.push_back(&b);
    }
    return result;
}

std::vector<Budget*> DataStore::get_budgets_for_entity_month(const std::string& entity_id, const std::string& month) {
    std::vector<Budget*> result;
    for (auto& b : budgets) {
        if (b.entity_id == entity_id && b.month == month) {
            result.push_back(&b);
        }
    }
    return result;
}

void DataStore::calculate_spent_amounts(const std::string& month) {
    for (auto& b : budgets) {
        if (b.month == month) {
            double spent = 0;
            for (auto& t : transactions) {
                if (t.date.substr(0, 7) == month && t.category_id == b.category_id && t.is_expense()) {
                    spent += std::abs(t.amount);
                }
            }
            b.spent_amount = spent;
        }
    }
}

double DataStore::get_total_net_worth() const {
    double total = 0;
    for (auto& acc : accounts) {
        if (acc.is_active) {
            if (acc.type == AccountType::CreditCard) {
                total -= acc.balance;
            } else {
                total += acc.balance;
            }
        }
    }
    return total;
}

double DataStore::get_total_net_worth_for_entity(const std::string& entity_id) const {
    double total = 0;
    for (auto& acc : accounts) {
        if (acc.entity_id == entity_id && acc.is_active) {
            if (acc.type == AccountType::CreditCard) {
                total -= acc.balance;
            } else {
                total += acc.balance;
            }
        }
    }
    return total;
}

double DataStore::get_total_balance_by_type(AccountType type) const {
    double total = 0;
    for (auto& acc : accounts) {
        if (acc.type == type && acc.is_active) {
            total += acc.balance;
        }
    }
    return total;
}

double DataStore::get_spending_for_month(const std::string& month) const {
    double total = 0;
    for (auto& t : transactions) {
        if (t.date.substr(0, 7) == month && t.is_expense()) {
            total += std::abs(t.amount);
        }
    }
    return total;
}

double DataStore::get_income_for_month(const std::string& month) const {
    double total = 0;
    for (auto& t : transactions) {
        if (t.date.substr(0, 7) == month && t.is_income()) {
            total += t.amount;
        }
    }
    return total;
}