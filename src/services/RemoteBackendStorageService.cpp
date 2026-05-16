// RemoteBackendStorageService.cpp — Phase 4.B implementation.
//
// load() strategy:
//   1. GET /entities  → build Entity list
//   2. For each entity:
//        GET /entities/<id>/accounts         → Account list
//        GET /entities/<id>/categories       → Category list
//        GET /entities/<id>/budgets          → Budget list
//   3. For each account:
//        GET /accounts/<id>/transactions     → Transaction list
//
// SECURITY: plaid_access_token was removed (4.C). Token management is via
// server-side PlaidTokenBroker. is_plaid_linked is populated from API response.
//
// save() — STUB: write-through not yet implemented (Phase 5).

#include "RemoteBackendStorageService.h"

#include "../models/Entity.h"
#include "../models/Account.h"
#include "../models/Transaction.h"
#include "../models/Category.h"
#include "../models/Budget.h"

#include <ctime>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

// --------------------------------------------------------------------------
// Helpers: JSON field → model struct
// --------------------------------------------------------------------------

static std::string unix_to_date_string(int64_t unix_ts) {
    if (unix_ts <= 0) return "";
    std::time_t t = static_cast<std::time_t>(unix_ts);
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return std::string(buf);
}

static Entity json_to_entity(const json& j) {
    Entity e;
    e.id         = j.value("id",   "");
    e.name       = j.value("name", "");
    std::string kind = j.value("kind", "individual");
    e.type       = Entity::type_from_string(kind);
    e.tax_id     = j.value("tax_id", "");
    e.is_active  = true;
    int64_t ts   = j.value("created_at_unix", int64_t{0});
    e.created_at = unix_to_date_string(ts);
    return e;
}

static Account json_to_account(const json& j) {
    Account a;
    a.id          = j.value("id",        "");
    a.name        = j.value("name",      "");
    a.entity_id   = j.value("entity_id", "");
    std::string kind = j.value("kind", "other");
    a.type        = Account::type_from_string(kind);
    // Server returns balance_cents (integer). Convert to double.
    int64_t cents = j.value("balance_cents", int64_t{0});
    a.balance     = static_cast<double>(cents) / 100.0;
    a.institution = j.value("institution", "");
    // plaid_item_id may be null in the JSON — treat null as empty string.
    if (j.contains("plaid_item_id") && j["plaid_item_id"].is_string()) {
        a.plaid_item_id = j["plaid_item_id"].get<std::string>();
    } else {
        a.plaid_item_id = "";
    }
    // plaid_access_token removed by 4.C — token is managed by server-side PlaidTokenBroker.
    // is_plaid_linked is populated from the JSON field below if present.
    a.is_plaid_linked = j.value("is_plaid_linked", false);
    a.is_active   = true;
    return a;
}

static Transaction json_to_transaction(const json& j) {
    Transaction t;
    t.id          = j.value("id",         "");
    t.account_id  = j.value("account_id", "");
    int64_t posted = j.value("posted_at_unix", int64_t{0});
    t.date        = unix_to_date_string(posted);
    int64_t cents = j.value("amount_cents", int64_t{0});
    t.amount      = static_cast<double>(cents) / 100.0;
    t.description = j.value("description", "");
    // category and plaid_transaction_id may be null — treat null as empty.
    if (j.contains("category") && j["category"].is_string()) {
        t.category_id = j["category"].get<std::string>();
    } else {
        t.category_id = "";
    }
    if (j.contains("plaid_transaction_id") && j["plaid_transaction_id"].is_string()) {
        t.plaid_transaction_id = j["plaid_transaction_id"].get<std::string>();
    } else {
        t.plaid_transaction_id = "";
    }
    t.pending     = false;
    t.notes       = "";
    t.check_number = "";
    return t;
}

static Category json_to_category(const json& j) {
    Category c;
    c.id        = j.value("id",   "");
    c.name      = j.value("name", "");
    std::string kind = j.value("kind", "expense");
    if (kind == "income") {
        c.type = CategoryType::Income;
    } else if (kind == "transfer") {
        c.type = CategoryType::Transfer;
    } else {
        c.type = CategoryType::Expense;
    }
    c.emoji     = "";
    c.parent_id = "";
    c.is_system = false;
    return c;
}

static Budget json_to_budget(const json& j) {
    Budget b;
    b.id          = j.value("id",        "");
    b.entity_id   = j.value("entity_id", "");
    // category_id may be null — treat null as empty.
    if (j.contains("category_id") && j["category_id"].is_string()) {
        b.category_id = j["category_id"].get<std::string>();
    } else {
        b.category_id = "";
    }
    // Server stores amount_cents; TUI model uses limit_amount as double.
    int64_t cents = j.value("amount_cents", int64_t{0});
    b.limit_amount = static_cast<double>(cents) / 100.0;
    b.spent_amount = 0.0;
    // Build month string from period_start_unix.
    int64_t ts    = j.value("period_start_unix", int64_t{0});
    if (ts > 0) {
        std::time_t t = static_cast<std::time_t>(ts);
        struct tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04d-%02d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1);
        b.month = std::string(buf);
    }
    return b;
}

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------

RemoteBackendStorageService::RemoteBackendStorageService(
    std::shared_ptr<BackendClient> backend,
    std::string session_token)
    : backend_(std::move(backend))
    , session_token_(std::move(session_token))
{}

// --------------------------------------------------------------------------
// fetch_list helper
// --------------------------------------------------------------------------

bool RemoteBackendStorageService::fetch_list(const std::string& path,
                                              json& out_items)
{
    auto result = backend_->get(path, session_token_);
    if (auto* err = std::get_if<BackendError>(&result)) {
        last_error_ = "fetch_list(" + path + "): " + err->message;
        return false;
    }
    const json& resp = std::get<json>(result);
    if (resp.contains("items") && resp["items"].is_array()) {
        out_items = resp["items"];
    } else if (resp.is_array()) {
        out_items = resp;
    } else {
        out_items = json::array();
    }
    return true;
}

// --------------------------------------------------------------------------
// load
// --------------------------------------------------------------------------

bool RemoteBackendStorageService::load(
    std::vector<Entity>& entities,
    std::vector<Account>& accounts,
    std::vector<Transaction>& transactions,
    std::vector<Category>& categories,
    std::vector<Budget>& budgets)
{
    entities.clear();
    accounts.clear();
    transactions.clear();
    categories.clear();
    budgets.clear();

    // Step 1: fetch entities.
    json entity_items;
    if (!fetch_list("/entities", entity_items)) return false;

    for (const auto& ej : entity_items) {
        Entity e = json_to_entity(ej);
        if (e.id.empty()) continue;
        entities.push_back(std::move(e));
    }

    // Step 2: per-entity sub-resources.
    for (const auto& e : entities) {
        // Accounts
        json acct_items;
        if (!fetch_list("/entities/" + e.id + "/accounts", acct_items)) return false;
        for (const auto& aj : acct_items) {
            Account a = json_to_account(aj);
            if (a.id.empty()) continue;
            accounts.push_back(a);
        }

        // Categories
        json cat_items;
        if (!fetch_list("/entities/" + e.id + "/categories", cat_items)) return false;
        for (const auto& cj : cat_items) {
            categories.push_back(json_to_category(cj));
        }

        // Budgets
        json bud_items;
        if (!fetch_list("/entities/" + e.id + "/budgets", bud_items)) return false;
        for (const auto& bj : bud_items) {
            budgets.push_back(json_to_budget(bj));
        }
    }

    // Step 3: per-account transactions.
    for (const auto& a : accounts) {
        json tx_items;
        if (!fetch_list("/accounts/" + a.id + "/transactions", tx_items)) return false;
        for (const auto& tj : tx_items) {
            transactions.push_back(json_to_transaction(tj));
        }
    }

    return true;
}

// --------------------------------------------------------------------------
// save — STUB (Phase 5 will implement write-through)
// --------------------------------------------------------------------------

bool RemoteBackendStorageService::save(
    const std::vector<Entity>&,
    const std::vector<Account>&,
    const std::vector<Transaction>&,
    const std::vector<Category>&,
    const std::vector<Budget>&)
{
    // Phase 4.B STUB: write-through to the server is deferred to Phase 5.
    // The TUI always fetches fresh data via load(); mutations go directly
    // through BackendClient calls in the service layer (Phase 5+).
    last_error_ = "RemoteBackendStorageService::save() is not implemented (Phase 5 stub)";
    return false;
}
