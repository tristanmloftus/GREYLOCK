#pragma once

#include <string>
#include <cstdint>

struct Transaction {
    std::string id;
    std::string account_id;
    std::string date;          // YYYY-MM-DD format
    double amount;            // Negative = expense, Positive = income
    std::string description;
    std::string category_id;
    bool pending = false;
    std::string plaid_transaction_id; // Populated when synced from Plaid
    std::string notes;        // User-added notes
    std::string check_number; // For checks

    bool is_expense() const { return amount < 0; }
    bool is_income() const { return amount > 0; }
};

struct TransactionFilter {
    std::string account_id;
    std::string category_id;
    std::string start_date;
    std::string end_date;
    double min_amount;
    double max_amount;
    bool show_pending_only = false;
    bool show_expenses_only = false;
    bool show_income_only = false;
    std::string search_text;
};