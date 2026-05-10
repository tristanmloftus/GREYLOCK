#include "StorageService.h"
#include <future>

JsonStorageService::JsonStorageService(const std::string& filepath)
    : filepath_(filepath) {}

namespace {
    void create_backup(const std::string& filepath) {
        std::ifstream src(filepath, std::ios::binary);
        if (!src.is_open()) return;

        std::string backup_path = filepath + ".backup";
        std::ofstream dst(backup_path, std::ios::binary);
        if (dst.is_open()) {
            dst << src.rdbuf();
        }
    }
}

bool JsonStorageService::load(
    std::vector<Entity>& entities,
    std::vector<Account>& accounts,
    std::vector<Transaction>& transactions,
    std::vector<Category>& categories,
    std::vector<Budget>& budgets
) {
    auto future_result = std::async(std::launch::async, [this, &entities, &accounts, &transactions, &categories, &budgets]() -> bool {
        std::ifstream file(filepath_, std::ios::binary);
        if (!file.is_open()) {
            last_error_ = "Could not open file: " + filepath_;
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        try {
            json j = json::parse(content);

            if (!parse_entities(j, entities)) return false;
            if (!parse_accounts(j, accounts)) return false;
            if (!parse_transactions(j, transactions)) return false;
            if (!parse_categories(j, categories)) return false;
            if (!parse_budgets(j, budgets)) return false;

            return true;
        } catch (const json::parse_error& e) {
            last_error_ = std::string("JSON parse error: ") + e.what();
            create_backup(filepath_);
            last_error_ += " (backup created)";
            return false;
        } catch (const std::exception& e) {
            last_error_ = std::string("Parse error: ") + e.what();
            create_backup(filepath_);
            last_error_ += " (backup created)";
            return false;
        }
    });

    return future_result.get();
}

bool JsonStorageService::parse_entities(const json& j, std::vector<Entity>& entities) {
    entities.clear();
    if (!j.contains("entities")) return true;

    for (const auto& e : j["entities"]) {
        Entity ent;
        ent.id = e.value("id", "");
        ent.name = e.value("name", "");
        ent.type = Entity::type_from_string(e.value("type", ""));
        ent.tax_id = e.value("tax_id", "");
        ent.is_active = e.value("is_active", true);
        ent.created_at = e.value("created_at", "");
        entities.push_back(ent);
    }
    return true;
}

bool JsonStorageService::parse_accounts(const json& j, std::vector<Account>& accounts) {
    accounts.clear();
    if (!j.contains("accounts")) return true;

    for (const auto& a : j["accounts"]) {
        Account acc;
        acc.id = a.value("id", "");
        acc.name = a.value("name", "");
        acc.entity_id = a.value("entity_id", "");
        acc.type = Account::type_from_string(a.value("type", ""));
        acc.balance = a.value("balance", 0.0);
        acc.institution = a.value("institution", "");
        acc.plaid_item_id = a.value("plaid_item_id", "");
        acc.is_plaid_linked = a.value("is_plaid_linked", false);
        acc.is_active = a.value("is_active", true);
        accounts.push_back(acc);
    }
    return true;
}

bool JsonStorageService::parse_transactions(const json& j, std::vector<Transaction>& transactions) {
    transactions.clear();
    if (!j.contains("transactions")) return true;

    for (const auto& t : j["transactions"]) {
        Transaction tx;
        tx.id = t.value("id", "");
        tx.account_id = t.value("account_id", "");
        tx.date = t.value("date", "");
        tx.amount = t.value("amount", 0.0);
        tx.description = t.value("description", "");
        tx.category_id = t.value("category_id", "");
        tx.pending = t.value("pending", false);
        tx.plaid_transaction_id = t.value("plaid_transaction_id", "");
        tx.notes = t.value("notes", "");
        tx.check_number = t.value("check_number", "");
        transactions.push_back(tx);
    }
    return true;
}

bool JsonStorageService::parse_categories(const json& j, std::vector<Category>& categories) {
    categories.clear();
    if (!j.contains("categories")) return true;

    for (const auto& c : j["categories"]) {
        Category cat;
        cat.id = c.value("id", "");
        cat.name = c.value("name", "");
        cat.type = (c.value("type", "expense") == "income") ? CategoryType::Income :
                   (c.value("type", "expense") == "transfer") ? CategoryType::Transfer :
                   CategoryType::Expense;
        cat.emoji = c.value("emoji", "");
        cat.parent_id = c.value("parent_id", "");
        cat.is_system = c.value("is_system", false);
        categories.push_back(cat);
    }
    return true;
}

bool JsonStorageService::parse_budgets(const json& j, std::vector<Budget>& budgets) {
    budgets.clear();
    if (!j.contains("budgets")) return true;

    for (const auto& b : j["budgets"]) {
        Budget bud;
        bud.id = b.value("id", "");
        bud.category_id = b.value("category_id", "");
        bud.month = b.value("month", "");
        bud.limit_amount = b.value("limit_amount", 0.0);
        bud.spent_amount = b.value("spent_amount", 0.0);
        budgets.push_back(bud);
    }
    return true;
}

bool JsonStorageService::save(
    const std::vector<Entity>& entities,
    const std::vector<Account>& accounts,
    const std::vector<Transaction>& transactions,
    const std::vector<Category>& categories,
    const std::vector<Budget>& budgets
) {
    auto future_result = std::async(std::launch::async, [this, &entities, &accounts, &transactions, &categories, &budgets]() -> bool {
        std::ofstream file(filepath_);
        if (!file.is_open()) {
            last_error_ = "Could not open file for writing: " + filepath_;
            return false;
        }

        try {
            json j;

            json entities_json = json::array();
            for (const auto& ent : entities) {
                entities_json.push_back({
                    {"id", ent.id},
                    {"name", ent.name},
                    {"type", ent.type_to_string()},
                    {"tax_id", ent.tax_id},
                    {"is_active", ent.is_active},
                    {"created_at", ent.created_at}
                });
            }
            j["entities"] = entities_json;

            json accounts_json = json::array();
            for (const auto& acc : accounts) {
                accounts_json.push_back({
                    {"id", acc.id},
                    {"name", acc.name},
                    {"entity_id", acc.entity_id},
                    {"type", acc.type_to_string()},
                    {"balance", acc.balance},
                    {"institution", acc.institution},
                    {"plaid_item_id", acc.plaid_item_id},
                    {"is_plaid_linked", acc.is_plaid_linked},
                    {"is_active", acc.is_active}
                });
            }
            j["accounts"] = accounts_json;

            json transactions_json = json::array();
            for (const auto& tx : transactions) {
                transactions_json.push_back({
                    {"id", tx.id},
                    {"account_id", tx.account_id},
                    {"date", tx.date},
                    {"amount", tx.amount},
                    {"description", tx.description},
                    {"category_id", tx.category_id},
                    {"pending", tx.pending},
                    {"plaid_transaction_id", tx.plaid_transaction_id},
                    {"notes", tx.notes},
                    {"check_number", tx.check_number}
                });
            }
            j["transactions"] = transactions_json;

            json categories_json = json::array();
            for (const auto& cat : categories) {
                std::string type_str = (cat.type == CategoryType::Income) ? "income" :
                                       (cat.type == CategoryType::Transfer) ? "transfer" : "expense";
                categories_json.push_back({
                    {"id", cat.id},
                    {"name", cat.name},
                    {"type", type_str},
                    {"emoji", cat.emoji},
                    {"parent_id", cat.parent_id},
                    {"is_system", cat.is_system}
                });
            }
            j["categories"] = categories_json;

            json budgets_json = json::array();
            for (const auto& bud : budgets) {
                budgets_json.push_back({
                    {"id", bud.id},
                    {"entity_id", bud.entity_id},
                    {"category_id", bud.category_id},
                    {"month", bud.month},
                    {"limit_amount", bud.limit_amount},
                    {"spent_amount", bud.spent_amount}
                });
            }
            j["budgets"] = budgets_json;

            file << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            last_error_ = std::string("Write error: ") + e.what();
            return false;
        }
    });

    return future_result.get();
}