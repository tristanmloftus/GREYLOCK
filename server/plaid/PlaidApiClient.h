#pragma once

// PlaidApiClient.h — Thin HTTP client for the Plaid API (Phase 4.D).
//
// DESIGN:
//   Wraps the Plaid REST API for transaction fetching.  All credentials
//   (PLAID_CLIENT_ID, PLAID_SECRET) are read from environment variables at
//   construction time.  They are NEVER returned to callers or logged.
//
// GUARDRAILS:
//   F-1: PLAID_CLIENT_ID and PLAID_SECRET from env vars only.
//   F-2: access_token is accepted as std::string_view (scoped lifetime);
//        it is never stored as a member variable.
//   F-3: Returns std::nullopt on any transport error or non-2xx response.
//        NEVER logs the access_token.
//   TLS verify: ON.  No insecure bypass.
//
// Transport: libcurl via IHttpClient.  Inject a mock for unit tests.
//
// Environment variables:
//   PLAID_CLIENT_ID   — required for any API call to succeed
//   PLAID_SECRET      — required for any API call to succeed
//   PLAID_ENV         — "sandbox" | "development" | "production" (default: sandbox)

#include "../../src/services/IHttpClient.h"

#include <sodium.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tf::plaid {

// ---------------------------------------------------------------------------
// PlaidTransaction — a single transaction returned by the Plaid API.
// ---------------------------------------------------------------------------
struct PlaidTransaction {
    std::string transaction_id;   // Plaid transaction_id
    std::string account_id;       // Plaid account_id (NOT our internal account id)
    int64_t     amount_cents{0};  // amount * 100, rounded (Plaid returns float dollars)
    int64_t     date_unix{0};     // posted date as Unix seconds (midnight UTC)
    std::string name;             // merchant / description name
    std::string category;         // top-level Plaid category (may be empty)
};

// ---------------------------------------------------------------------------
// TransactionList — result from sync_transactions / fetch_transactions.
//
// For /transactions/sync: added/modified/removed + next_cursor.
// For /transactions/get:  all results go into `added`.
// ---------------------------------------------------------------------------
struct TransactionList {
    std::vector<PlaidTransaction> added;
    std::vector<PlaidTransaction> modified;
    std::vector<std::string>      removed_ids;  // transaction_id of removed txns
    std::string                   next_cursor;  // pagination cursor for /sync
    bool                          has_more{false};
};

// ---------------------------------------------------------------------------
// PlaidApiClient
//
// Thread safety: NOT thread-safe.  Each sync worker should hold its own
// instance (or external locking must be provided).
// ---------------------------------------------------------------------------
class PlaidApiClient {
public:
    // Construct with injected HTTP client.  Reads PLAID_CLIENT_ID,
    // PLAID_SECRET, and PLAID_ENV from the environment.  Does NOT throw if
    // the env vars are absent — callers can check credentials_available() to
    // determine whether calls will succeed.
    //
    // http_client must outlive this object.
    explicit PlaidApiClient(IHttpClient& http_client);

    // RC-2: Zero heap memory holding PLAID_CLIENT_ID and PLAID_SECRET on
    // destruction.  std::string's destructor deallocates but does not zero,
    // leaving credentials in freed memory.  sodium_memzero() is guaranteed not
    // to be elided by the optimizer (unlike memset).
    virtual ~PlaidApiClient() {
        sodium_memzero(client_id_.data(), client_id_.size());
        sodium_memzero(secret_.data(), secret_.size());
    }

    // Non-copyable / non-movable (holds a reference to IHttpClient).
    PlaidApiClient(const PlaidApiClient&) = delete;
    PlaidApiClient& operator=(const PlaidApiClient&) = delete;
    PlaidApiClient(PlaidApiClient&&) = delete;
    PlaidApiClient& operator=(PlaidApiClient&&) = delete;

    // Returns true if PLAID_CLIENT_ID and PLAID_SECRET were set at construction.
    bool credentials_available() const noexcept;

    // -----------------------------------------------------------------------
    // sync_transactions — POST /transactions/sync
    //
    // Incremental transaction sync (preferred for polling schedulers).
    // access_token: Plaid access token — NEVER stored or logged.
    // cursor: nullopt for first call; use TransactionList::next_cursor on
    //         subsequent calls to fetch only new deltas.
    //
    // Returns nullopt on transport error or non-2xx response.
    // GUARDRAIL: access_token is NEVER included in any log message.
    // -----------------------------------------------------------------------
    virtual std::optional<TransactionList> sync_transactions(
        std::string_view access_token,
        std::optional<std::string> cursor);

    // -----------------------------------------------------------------------
    // fetch_transactions — POST /transactions/get
    //
    // Classic date-range query.  Results are returned in `added` only.
    // from_unix / to_unix: Unix timestamps (seconds); converted to YYYY-MM-DD.
    //
    // Returns nullopt on transport error or non-2xx response.
    // GUARDRAIL: access_token is NEVER included in any log message.
    // -----------------------------------------------------------------------
    virtual std::optional<TransactionList> fetch_transactions(
        std::string_view access_token,
        std::string_view account_id,
        int64_t from_unix,
        int64_t to_unix);

protected:
    // Protected for mock override in tests.
    PlaidApiClient() = default;

private:
    // Build the base URL for the configured environment.
    // "sandbox"     → https://sandbox.plaid.com
    // "development" → https://development.plaid.com
    // "production"  → https://production.plaid.com
    std::string base_url() const;

    // Build the common JSON request body fields (client_id, secret).
    // GUARDRAIL: the returned string contains credentials — NEVER log it.
    std::string build_auth_fields() const;

    // Parse a Plaid date string "YYYY-MM-DD" to Unix seconds (midnight UTC).
    static int64_t parse_plaid_date(const std::string& date_str);

    // Format a Unix timestamp as "YYYY-MM-DD".
    static std::string format_plaid_date(int64_t unix_secs);

    IHttpClient* http_client_{nullptr};  // non-owning; may be null for default-constructed mock

    // Credentials — stored only as long as this object lives.
    // GUARDRAIL: never returned via accessor; never logged.
    std::string client_id_;
    std::string secret_;
    std::string env_;  // "sandbox" | "development" | "production"
};

} // namespace tf::plaid
