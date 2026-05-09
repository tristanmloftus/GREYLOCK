#pragma once

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <optional>
#include <algorithm>
#include <memory>

#include "Account.h"
#include "Transaction.h"
#include "Category.h"
#include "Budget.h"
#include "Entity.h"
#include "../services/StorageService.h"

class DataStore {
public:
    explicit DataStore(std::shared_ptr<IStorageService> storage = nullptr);

    // Entity operations
    Entity& add_entity(Entity entity);
    void remove_entity(const std::string& entity_id);
    std::optional<Entity*> get_entity(const std::string& entity_id);
    std::vector<Entity*> get_all_entities();

    // Account operations
    Account& add_account(Account account);
    void remove_account(const std::string& account_id);
    std::optional<Account*> get_account(const std::string& account_id);
    std::vector<Account*> get_accounts_by_type(AccountType type);
    std::vector<Account*> get_accounts_for_entity(const std::string& entity_id);

    // Transaction operations
    Transaction& add_transaction(Transaction transaction);
    void remove_transaction(const std::string& transaction_id);
    std::optional<Transaction*> get_transaction(const std::string& transaction_id);
    std::vector<Transaction*> get_transactions_for_account(const std::string& account_id);
    std::vector<Transaction*> get_transactions_for_entity(const std::string& entity_id);
    std::vector<Transaction*> filter_transactions(const TransactionFilter& filter);
    void update_transaction_category(const std::string& transaction_id, const std::string& category_id);

    // Category operations
    void init_default_categories();
    std::optional<Category*> get_category(const std::string& category_id);
    std::vector<Category*> get_expense_categories();
    std::vector<Category*> get_income_categories();
    Category& add_category(Category category);

    // Budget operations
    Budget& add_budget(Budget budget);
    std::optional<Budget*> get_budget(const std::string& category_id, const std::string& month);
    std::vector<Budget*> get_budgets_for_month(const std::string& month);
    std::vector<Budget*> get_budgets_for_entity_month(const std::string& entity_id, const std::string& month);
    void calculate_spent_amounts(const std::string& month);

    // Persistence
    bool load();
    bool save();

    // Summary calculations
    double get_total_net_worth() const;
    double get_total_net_worth_for_entity(const std::string& entity_id) const;
    double get_total_balance_by_type(AccountType type) const;
    double get_spending_for_month(const std::string& month) const;
    double get_income_for_month(const std::string& month) const;

    void set_storage(std::shared_ptr<IStorageService> storage);
    std::string get_last_error() const;

    // Data vectors
    std::vector<Entity> entities;
    std::vector<Account> accounts;
    std::vector<Transaction> transactions;
    std::vector<Category> categories;
    std::vector<Budget> budgets;

private:
    std::shared_ptr<IStorageService> storage_;
    std::string last_error_;
    std::string generate_id();
};