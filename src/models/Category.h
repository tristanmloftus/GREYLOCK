#pragma once

#include <string>
#include <vector>

enum class CategoryType {
    Expense,
    Income,
    Transfer
};

struct Category {
    std::string id;
    std::string name;
    CategoryType type;
    std::string emoji;        // For UI display
    std::string parent_id;   // Empty for top-level, set for subcategories
    bool is_system = false;  // true = built-in, false = user-created

    bool is_subcategory() const { return !parent_id.empty(); }
};

const std::vector<Category> DEFAULT_CATEGORIES = {
    // Expense categories
    {"cat_food", "Food & Dining", CategoryType::Expense, "🍔", "", true},
    {"cat_groceries", "Groceries", CategoryType::Expense, "🛒", "cat_food", true},
    {"cat_restaurants", "Restaurants", CategoryType::Expense, "🍽️", "cat_food", true},
    {"cat_transport", "Transportation", CategoryType::Expense, "🚗", "", true},
    {"cat_gas", "Gas", CategoryType::Expense, "⛽", "cat_transport", true},
    {"cat_utilities", "Utilities", CategoryType::Expense, "💡", "", true},
    {"cat_housing", "Housing", CategoryType::Expense, "🏠", "", true},
    {"cat_rent", "Rent/Mortgage", CategoryType::Expense, "🏦", "cat_housing", true},
    {"cat_health", "Healthcare", CategoryType::Expense, "🏥", "", true},
    {"cat_entertainment", "Entertainment", CategoryType::Expense, "🎬", "", true},
    {"cat_shopping", "Shopping", CategoryType::Expense, "🛍️", "", true},
    {"cat_subscriptions", "Subscriptions", CategoryType::Expense, "📱", "", true},
    {"cat_other_expense", "Other", CategoryType::Expense, "📦", "", true},

    // Income categories
    {"cat_salary", "Salary", CategoryType::Income, "💼", "", true},
    {"cat_freelance", "Freelance", CategoryType::Income, "💻", "", true},
    {"cat_investment_income", "Investment", CategoryType::Income, "📈", "", true},
    {"cat_other_income", "Other Income", CategoryType::Income, "💰", "", true},

    // Transfer
    {"cat_transfer", "Transfer", CategoryType::Transfer, "↔️", "", true},
};