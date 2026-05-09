#pragma once

#include <string>
#include <vector>
#include <random>

#include "../models/Transaction.h"
#include "../models/Account.h"
#include "../models/Category.h"

class SyntheticGenerator {
public:
    static std::vector<Transaction> generateTransactions(const std::string& account_id, size_t count);
    static std::vector<Account> generateAccounts(size_t count);
    static std::vector<Category> generateCategories(size_t count);
};