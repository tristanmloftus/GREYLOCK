#include "V01Migrator.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "../services/BackendClient.h"

// --------------------------------------------------------------------------
// BackendClientAdapter — maps BackendError → MigrationBackendError
// --------------------------------------------------------------------------

std::variant<json, MigrationBackendError> BackendClientAdapter::post_migration(
    const std::string& path,
    const json& body,
    const std::string& session_token)
{
    auto result = client_.post(path, body, session_token);

    if (std::holds_alternative<json>(result)) {
        return std::get<json>(result);
    }

    const auto& err = std::get<BackendError>(result);

    MigrationBackendError::Kind kind;
    switch (err.kind) {
        case BackendError::Kind::Transport:    kind = MigrationBackendError::Kind::Transport;    break;
        case BackendError::Kind::Conflict:     kind = MigrationBackendError::Kind::Conflict;     break;
        case BackendError::Kind::NotFound:     kind = MigrationBackendError::Kind::NotFound;     break;
        case BackendError::Kind::ServerError:  kind = MigrationBackendError::Kind::ServerError;  break;
        case BackendError::Kind::Unauthorized: kind = MigrationBackendError::Kind::Unauthorized; break;
        case BackendError::Kind::BadResponse:  [[fallthrough]];
        case BackendError::Kind::RateLimited:  [[fallthrough]];
        default:                               kind = MigrationBackendError::Kind::BadResponse;  break;
    }

    return MigrationBackendError{kind, err.http_status, err.code, err.message};
}

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------

V01Migrator::V01Migrator(IBackendClient& client, std::string session_token)
    : client_(client)
    , session_token_(std::move(session_token))
{}

// --------------------------------------------------------------------------
// migrate()
// --------------------------------------------------------------------------

MigrationReport V01Migrator::migrate(const std::filesystem::path& json_path) {
    MigrationReport report;

    // Open and read the file.
    std::ifstream file(json_path);
    if (!file.is_open()) {
        report.errors++;
        report.error_messages.push_back(
            "Cannot open file: " + json_path.string());
        return report;
    }

    std::string content(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    json j;
    try {
        j = json::parse(content);
    } catch (const json::parse_error& e) {
        report.errors++;
        report.error_messages.push_back(
            std::string("JSON parse error: ") + e.what());
        return report;
    }

    // Walk each collection.
    migrate_entities(j, report);
    migrate_accounts(j, report);
    migrate_transactions(j, report);
    migrate_categories(j, report);
    migrate_budgets(j, report);

    return report;
}

// --------------------------------------------------------------------------
// Private: per-collection migration
// --------------------------------------------------------------------------

void V01Migrator::migrate_entities(const json& j, MigrationReport& report) {
    if (!j.contains("entities") || !j["entities"].is_array()) return;

    const auto& arr = j["entities"];
    const int total = static_cast<int>(arr.size());
    int idx = 0;

    for (const auto& e : arr) {
        ++idx;
        std::string id   = e.value("id", "");
        std::string name = e.value("name", "");

        std::cout << "Migrating entity " << id
                  << " (" << idx << "/" << total << ")..." << std::flush;

        // Build server POST body (fields the v0.2 server schema accepts).
        json body;
        body["id"]         = id;
        body["name"]       = name;
        body["kind"]       = e.value("type", "other");     // v0.1: "type" → server: "kind"
        body["tax_id"]     = e.value("tax_id", "");
        body["is_active"]  = e.value("is_active", true);
        body["created_at"] = e.value("created_at", "");

        auto result = client_.post_migration("/entities", body, session_token_);
        bool created = handle_post_result(result, "entity", id,
                                          report.entities_created,
                                          report.entities_skipped,
                                          report);
        if (created) {
            std::cout << " created.\n";
        }
    }

    std::cout << "Entities: " << report.entities_created << " created, "
              << report.entities_skipped << " skipped.\n";
}

void V01Migrator::migrate_accounts(const json& j, MigrationReport& report) {
    if (!j.contains("accounts") || !j["accounts"].is_array()) return;

    const auto& arr = j["accounts"];
    const int total = static_cast<int>(arr.size());
    int idx = 0;

    for (const auto& a : arr) {
        ++idx;
        std::string id        = a.value("id", "");
        std::string entity_id = a.value("entity_id", "");

        std::cout << "Migrating account " << id
                  << " (" << idx << "/" << total << ")..." << std::flush;

        // Build POST body — NOTE: plaid_access_token is intentionally OMITTED
        // (guardrail F-3: removed from server schema; re-link via --link-plaid).
        json body;
        body["id"]           = id;
        body["name"]         = a.value("name", "");
        body["type"]         = a.value("type", "other");
        body["institution"]  = a.value("institution", "");
        body["plaid_item_id"] = a.value("plaid_item_id", "");
        body["is_active"]    = a.value("is_active", true);
        // plaid_access_token deliberately not included

        std::string path = "/entities/" + entity_id + "/accounts";
        auto result = client_.post_migration(path, body, session_token_);
        bool created = handle_post_result(result, "account", id,
                                          report.accounts_created,
                                          report.accounts_skipped,
                                          report);
        if (created) {
            std::cout << " created.\n";
        }
    }

    std::cout << "Accounts: " << report.accounts_created << " created, "
              << report.accounts_skipped << " skipped.\n";
}

void V01Migrator::migrate_transactions(const json& j, MigrationReport& report) {
    if (!j.contains("transactions") || !j["transactions"].is_array()) return;

    const auto& arr = j["transactions"];
    const int total = static_cast<int>(arr.size());
    int idx = 0;

    for (const auto& t : arr) {
        ++idx;
        std::string id         = t.value("id", "");
        std::string account_id = t.value("account_id", "");

        // Log ID only — do not log description, amount, or other user-data fields.
        std::cout << "Migrating transaction " << id
                  << " (" << idx << "/" << total << ")..." << std::flush;

        // Build POST body — description is included in the payload (server
        // stores it) but is NOT printed to stdout per privacy guardrail.
        json body;
        body["id"]                    = id;
        body["date"]                  = t.value("date", "");
        body["amount"]                = t.value("amount", 0.0);
        body["description"]           = t.value("description", "");
        body["category_id"]           = t.value("category_id", "");
        body["pending"]               = t.value("pending", false);
        body["plaid_transaction_id"]  = t.value("plaid_transaction_id", "");
        body["notes"]                 = t.value("notes", "");
        body["check_number"]          = t.value("check_number", "");

        std::string path = "/accounts/" + account_id + "/transactions";
        // Include account_id in the resource identifier for error messages so
        // failures clearly indicate which account was missing on the server.
        std::string resource_id = id + " (account=" + account_id + ")";
        auto result = client_.post_migration(path, body, session_token_);
        bool created = handle_post_result(result, "transaction", resource_id,
                                          report.transactions_created,
                                          report.transactions_skipped,
                                          report);
        if (created) {
            std::cout << " created.\n";
        }
    }

    std::cout << "Transactions: " << report.transactions_created << " created, "
              << report.transactions_skipped << " skipped.\n";
}

void V01Migrator::migrate_categories(const json& j, MigrationReport& report) {
    if (!j.contains("categories") || !j["categories"].is_array()) return;

    const auto& arr = j["categories"];
    const int total = static_cast<int>(arr.size());
    int idx = 0;

    for (const auto& c : arr) {
        ++idx;
        std::string id = c.value("id", "");

        std::cout << "Migrating category " << id
                  << " (" << idx << "/" << total << ")..." << std::flush;

        json body;
        body["id"]        = id;
        body["name"]      = c.value("name", "");
        body["type"]      = c.value("type", "expense");
        body["emoji"]     = c.value("emoji", "");
        body["parent_id"] = c.value("parent_id", "");
        body["is_system"] = c.value("is_system", false);

        auto result = client_.post_migration("/categories", body, session_token_);
        bool created = handle_post_result(result, "category", id,
                                          report.categories_created,
                                          report.categories_skipped,
                                          report);
        if (created) {
            std::cout << " created.\n";
        }
    }

    std::cout << "Categories: " << report.categories_created << " created, "
              << report.categories_skipped << " skipped.\n";
}

void V01Migrator::migrate_budgets(const json& j, MigrationReport& report) {
    if (!j.contains("budgets") || !j["budgets"].is_array()) return;

    const auto& arr = j["budgets"];
    const int total = static_cast<int>(arr.size());
    int idx = 0;

    for (const auto& b : arr) {
        ++idx;
        std::string id = b.value("id", "");

        std::cout << "Migrating budget " << id
                  << " (" << idx << "/" << total << ")..." << std::flush;

        json body;
        body["id"]           = id;
        body["entity_id"]    = b.value("entity_id", "");
        body["category_id"]  = b.value("category_id", "");
        body["month"]        = b.value("month", "");
        body["limit_amount"] = b.value("limit_amount", 0.0);
        // spent_amount is calculated server-side; do not migrate it.

        auto result = client_.post_migration("/budgets", body, session_token_);
        bool created = handle_post_result(result, "budget", id,
                                          report.budgets_created,
                                          report.budgets_skipped,
                                          report);
        if (created) {
            std::cout << " created.\n";
        }
    }

    std::cout << "Budgets: " << report.budgets_created << " created, "
              << report.budgets_skipped << " skipped.\n";
}

// --------------------------------------------------------------------------
// Private: result handler
// --------------------------------------------------------------------------

bool V01Migrator::handle_post_result(
    std::variant<json, MigrationBackendError> result,
    const std::string& resource_kind,
    const std::string& resource_id,
    int& created_count,
    int& skipped_count,
    MigrationReport& report)
{
    if (std::holds_alternative<json>(result)) {
        ++created_count;
        return true;
    }

    const auto& err = std::get<MigrationBackendError>(result);

    if (err.kind == MigrationBackendError::Kind::Conflict) {
        // 409 — already exists; idempotent skip.
        ++skipped_count;
        std::cout << " skipped (already exists).\n";
        return false;
    }

    // Any other error: record it and continue.
    ++report.errors;
    std::ostringstream msg;
    msg << resource_kind << " " << resource_id
        << ": HTTP " << err.http_status
        << " [" << err.code << "] " << err.message;
    report.error_messages.push_back(msg.str());
    std::cout << " ERROR: " << err.code << " (HTTP " << err.http_status << ")\n";
    return false;
}
