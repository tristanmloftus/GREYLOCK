#include "SyntheticGenerator.h"
#include "Logger.h"
#include "../models/Account.h"

std::vector<Transaction> SyntheticGenerator::generateTransactions(
    const std::string& account_id,
    size_t count
) {
    std::vector<Transaction> transactions;
    transactions.reserve(count);

    const std::vector<std::pair<std::string, std::string>> merchant_categories = {
        {"Amazon Web Services", "cat_subscriptions"},
        {"Whole Foods Market", "cat_groceries"},
        {"Starbucks Coffee", "cat_restaurants"},
        {"Shell Gas Station", "cat_gas"},
        {"Netflix Subscription", "cat_subscriptions"},
        {"Spotify Premium", "cat_subscriptions"},
        {"Uber Ride", "cat_transport"},
        {"DoorDash Delivery", "cat_restaurants"},
        {"Target", "cat_shopping"},
        {"Home Depot", "cat_housing"},
        {"Chipotle Mexican Grill", "cat_restaurants"},
        {"AT&T Wireless", "cat_utilities"},
        {"Comcast Xfinity", "cat_utilities"},
        {"Adobe Creative Cloud", "cat_subscriptions"},
        {"Google Cloud Platform", "cat_subscriptions"},
        {"Microsoft Azure", "cat_subscriptions"},
        {"Payroll Deposit - Acme Corp", "cat_salary"},
        {"Venmo Transfer", "cat_other_expense"},
        {"Mortgage Payment - Wells Fargo", "cat_rent"},
        {"Best Buy Electronics", "cat_shopping"}
    };

    const std::vector<std::pair<double, double>> amount_ranges = {
        {-5.0, -50.0},
        {-50.0, -150.0},
        {-150.0, -500.0},
        {-500.0, -2000.0},
        {100.0, 5000.0}
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> month_dist(1, 5);
    std::uniform_int_distribution<> day_dist(1, 28);
    std::uniform_int_distribution<> merchant_dist(0, static_cast<int>(merchant_categories.size()) - 1);
    std::uniform_int_distribution<> amount_type_dist(0, 100);

    for (size_t i = 0; i < count; ++i) {
        Transaction tx;
        tx.id = "tx_synthetic_" + std::to_string(i);
        tx.account_id = account_id;
        tx.pending = (i % 20 == 0);

        int month = month_dist(gen);
        int day = day_dist(gen);
        tx.date = "2026-" + std::string(month < 10 ? "0" : "") + std::to_string(month) + "-" +
                  std::string(day < 10 ? "0" : "") + std::to_string(day);

        auto [desc, cat] = merchant_categories[merchant_dist(gen)];
        tx.description = desc;
        tx.category_id = cat;

        int amount_type = amount_type_dist(gen);
        std::uniform_real_distribution<> amount_gen;

        if (amount_type < 10) {
            amount_gen = std::uniform_real_distribution<>(amount_ranges[0].first, amount_ranges[0].second);
        } else if (amount_type < 30) {
            amount_gen = std::uniform_real_distribution<>(amount_ranges[1].first, amount_ranges[1].second);
        } else if (amount_type < 60) {
            amount_gen = std::uniform_real_distribution<>(amount_ranges[2].first, amount_ranges[2].second);
        } else if (amount_type < 85) {
            amount_gen = std::uniform_real_distribution<>(amount_ranges[3].first, amount_ranges[3].second);
        } else {
            amount_gen = std::uniform_real_distribution<>(amount_ranges[4].first, amount_ranges[4].second);
        }

        tx.amount = std::round(amount_gen(gen) * 100.0) / 100.0;

        if (tx.description.find("Payroll") != std::string::npos || 
            tx.description.find("Deposit") != std::string::npos) {
            tx.amount = std::abs(tx.amount);
        }

        transactions.push_back(tx);
    }

    Logger::instance().info("SyntheticGenerator: Generated " + std::to_string(count) + " transactions");
    return transactions;
}

std::vector<Account> SyntheticGenerator::generateAccounts(size_t count) {
    std::vector<Account> accounts;
    accounts.reserve(count);

    const std::vector<std::pair<std::string, AccountType>> account_templates = {
        {"Personal Checking", AccountType::Checking},
        {"Personal Savings", AccountType::Savings},
        {"Chase Credit Card", AccountType::CreditCard},
        {"Investment Portfolio", AccountType::Investment},
        {"Business Checking", AccountType::Checking},
        {"Business Savings", AccountType::Savings},
        {"Business Credit Card", AccountType::CreditCard}
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> balance_dist(-5000.0, 50000.0);

    for (size_t i = 0; i < count; ++i) {
        Account acc;
        acc.id = "acc_synthetic_" + std::to_string(i);
        
        auto [name, type] = account_templates[i % account_templates.size()];
        acc.name = name + " #" + std::to_string(i + 1);
        acc.type = type;
        acc.balance = std::round(balance_dist(gen) * 100.0) / 100.0;
        acc.institution = "Test Bank";
        acc.is_active = true;

        accounts.push_back(acc);
    }

    Logger::instance().info("SyntheticGenerator: Generated " + std::to_string(count) + " accounts");
    return accounts;
}

std::vector<Category> SyntheticGenerator::generateCategories(size_t count) {
    std::vector<Category> categories;
    categories.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        Category cat;
        cat.id = "cat_custom_" + std::to_string(i);
        cat.name = "Custom Category " + std::to_string(i);
        cat.type = CategoryType::Expense;
        cat.emoji = "📁";
        cat.is_system = false;
        categories.push_back(cat);
    }

    Logger::instance().info("SyntheticGenerator: Generated " + std::to_string(count) + " categories");
    return categories;
}