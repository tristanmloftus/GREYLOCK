#pragma once

#include <string>
#include <cstdint>

enum class AccountType {
    Checking,
    Savings,
    CreditCard,
    Investment,
    Cash,
    Other
};

struct Account {
    std::string id;
    std::string name;
    std::string entity_id;           // Links to Entity (family office, holding company, personal)
    AccountType type;
    double balance;
    std::string institution;
    std::string plaid_item_id;       // Populated when linked via Plaid
    bool is_plaid_linked = false;   // True when an encrypted token is stored server-side
    bool is_active = true;

    std::string type_to_string() const;
    static AccountType type_from_string(const std::string& s);
};

inline std::string Account::type_to_string() const {
    switch (type) {
        case AccountType::Checking: return "checking";
        case AccountType::Savings: return "savings";
        case AccountType::CreditCard: return "credit_card";
        case AccountType::Investment: return "investment";
        case AccountType::Cash: return "cash";
        default: return "other";
    }
}

inline AccountType Account::type_from_string(const std::string& s) {
    if (s == "checking") return AccountType::Checking;
    if (s == "savings") return AccountType::Savings;
    if (s == "credit_card") return AccountType::CreditCard;
    if (s == "investment") return AccountType::Investment;
    if (s == "cash") return AccountType::Cash;
    return AccountType::Other;
}