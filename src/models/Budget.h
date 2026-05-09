#pragma once

#include <string>
#include <vector>

struct Budget {
    std::string id;
    std::string entity_id;     // Links to Entity
    std::string category_id;
    std::string month;        // YYYY-MM format
    double limit_amount;
    double spent_amount = 0; // Calculated, not stored

    double remaining() const { return limit_amount - spent_amount; }
    double percent_used() const {
        if (limit_amount == 0) return 0;
        return (spent_amount / limit_amount) * 100.0;
    }
};

struct MonthlyBudget {
    std::string month;          // YYYY-MM
    double total_income;
    double total_budgeted;
    double total_spent;
    double total_available;
    std::vector<Budget> category_budgets;
};