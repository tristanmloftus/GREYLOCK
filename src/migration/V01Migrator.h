#pragma once

// V01Migrator.h — One-shot tool that reads a v0.1 JSON file (JsonStorageService format)
// and uploads its contents to the server via IBackendClient.
//
// Idempotent: a 409 (Conflict) response from the server means the row already exists;
// the migrator counts it as skipped rather than an error.
//
// Fields NOT migrated (guardrail F-3 compliance):
//   - Account.plaid_access_token  — removed from server schema; re-link via --link-plaid
//   - Existing audit log entries  — there were none in v0.1
//
// Privacy guardrails:
//   - Transaction descriptions, account balances, and other user-data fields
//     are NEVER logged. Only counts and IDs appear in progress output.
//
// Migration header ("X-TF-Migration: 1") is set on every POST so the server-side
// audit log records the actor_kind as "migration" rather than "user".

#include <filesystem>
#include <string>
#include <vector>
#include <variant>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// --------------------------------------------------------------------------
// Structured error type (mirrors BackendError from Phase 4.A)
// --------------------------------------------------------------------------

struct MigrationBackendError {
    enum class Kind {
        Transport,    // network failure
        Conflict,     // 409 — row already exists (idempotent skip, not an error)
        NotFound,     // 404 — referenced resource doesn't exist (e.g. account for a tx)
        ServerError,  // 5xx
        BadResponse,  // malformed JSON or unexpected 4xx
        Unauthorized, // 401
    };

    Kind        kind;
    long        http_status;  // 0 for Transport
    std::string code;
    std::string message;
};

// --------------------------------------------------------------------------
// Minimal IBackendClient interface for V01Migrator
// The concrete BackendClient (Phase 4.A) will implement this via BackendClientAdapter.
// FakeBackendClient in tests also implements it.
// --------------------------------------------------------------------------

class IBackendClient {
public:
    virtual ~IBackendClient() = default;

    // POST <path> with JSON body and the migration header.
    // Returns parsed JSON on 200/201.
    // Returns MigrationBackendError on failure.
    virtual std::variant<json, MigrationBackendError> post_migration(
        const std::string& path,
        const json& body,
        const std::string& session_token) = 0;
};

// --------------------------------------------------------------------------
// BackendClientAdapter
//
// Wraps the real BackendClient (Phase 2.C) and forwards post_migration() calls
// to BackendClient::post().
//
// Note on X-TF-Migration header: BackendClient's public post() API does not
// support per-call custom headers. The header is a server-side audit hint and
// is deferred to a future phase when BackendClient gains a headers overload.
// The migration logic (idempotency, privacy guardrails) is fully preserved.
// --------------------------------------------------------------------------

// Forward-declare to avoid including BackendClient.h in every consumer of this header.
class BackendClient;

class BackendClientAdapter : public IBackendClient {
public:
    explicit BackendClientAdapter(BackendClient& client) : client_(client) {}

    std::variant<json, MigrationBackendError> post_migration(
        const std::string& path,
        const json& body,
        const std::string& session_token) override;

private:
    BackendClient& client_;
};

// --------------------------------------------------------------------------
// MigrationReport
// --------------------------------------------------------------------------

struct MigrationReport {
    int entities_created    = 0;
    int entities_skipped    = 0;   // 409 responses
    int accounts_created    = 0;
    int accounts_skipped    = 0;
    int transactions_created = 0;
    int transactions_skipped = 0;
    int categories_created  = 0;
    int categories_skipped  = 0;
    int budgets_created     = 0;
    int budgets_skipped     = 0;
    int errors              = 0;
    std::vector<std::string> error_messages;
};

// --------------------------------------------------------------------------
// V01Migrator
// --------------------------------------------------------------------------

class V01Migrator {
public:
    V01Migrator(IBackendClient& client, std::string session_token);

    // Read the v0.1 JSON file at json_path, walk entities/accounts/transactions/
    // categories/budgets, POST each to the server.
    // Progress is printed to stdout (counts and IDs only — no user-data values).
    // Returns a MigrationReport.
    MigrationReport migrate(const std::filesystem::path& json_path);

private:
    void migrate_entities(const json& j, MigrationReport& report);
    void migrate_accounts(const json& j, MigrationReport& report);
    void migrate_transactions(const json& j, MigrationReport& report);
    void migrate_categories(const json& j, MigrationReport& report);
    void migrate_budgets(const json& j, MigrationReport& report);

    // Handle the result of a single POST; returns true if created, false if skipped (409),
    // and increments report.errors + appends message on any other failure.
    // created_count and skipped_count are incremented in place.
    bool handle_post_result(
        std::variant<json, MigrationBackendError> result,
        const std::string& resource_kind,
        const std::string& resource_id,
        int& created_count,
        int& skipped_count,
        MigrationReport& report);

    IBackendClient& client_;
    std::string session_token_;
};
