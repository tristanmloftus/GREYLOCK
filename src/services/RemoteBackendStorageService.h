#pragma once

// RemoteBackendStorageService.h — IStorageService backed by the Greylock
// server API (Phase 4.B).
//
// load()  — fetches all data from server via BackendClient.
// save()  — STUB, always returns false (write-through deferred to Phase 5).

#include "StorageService.h"
#include "BackendClient.h"

#include <memory>
#include <string>
#include <vector>

class RemoteBackendStorageService : public IStorageService {
public:
    // backend: shared BackendClient configured with the server base URL.
    // session_token: bearer token from the most recent successful login.
    RemoteBackendStorageService(std::shared_ptr<BackendClient> backend,
                                std::string session_token);

    // Fetch all entities, accounts, transactions, categories, and budgets
    // from the server.  Populates all five output vectors.
    // Returns true on success, false on any transport/server error.
    bool load(
        std::vector<Entity>& entities,
        std::vector<Account>& accounts,
        std::vector<Transaction>& transactions,
        std::vector<Category>& categories,
        std::vector<Budget>& budgets
    ) override;

    // STUB — Phase 5 will implement write-through to the server.
    // Always returns false with a documented "not implemented" reason.
    bool save(
        const std::vector<Entity>& entities,
        const std::vector<Account>& accounts,
        const std::vector<Transaction>& transactions,
        const std::vector<Category>& categories,
        const std::vector<Budget>& budgets
    ) override;

    std::string get_last_error() const override { return last_error_; }

private:
    // Fetch a paginated list from `path` using GET.
    // On success, sets out_items to the "items" array and returns true.
    // On failure, sets last_error_ and returns false.
    bool fetch_list(const std::string& path, nlohmann::json& out_items);

    std::shared_ptr<BackendClient> backend_;
    std::string session_token_;
    mutable std::string last_error_;
};
