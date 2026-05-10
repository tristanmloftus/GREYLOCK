// PlaidApiClient.cpp — Thin HTTP client for the Plaid API (Phase 4.D).
//
// See PlaidApiClient.h for design notes, guardrails, and security contract.

#include "PlaidApiClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace tf::plaid {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
PlaidApiClient::PlaidApiClient(IHttpClient& http_client)
    : http_client_(&http_client)
{
    const char* cid = std::getenv("PLAID_CLIENT_ID");
    const char* sec = std::getenv("PLAID_SECRET");
    const char* env = std::getenv("PLAID_ENV");

    client_id_ = (cid && cid[0] != '\0') ? std::string(cid) : "";
    secret_    = (sec && sec[0] != '\0') ? std::string(sec) : "";
    env_       = (env && env[0] != '\0') ? std::string(env) : "sandbox";
}

// ---------------------------------------------------------------------------
// credentials_available
// ---------------------------------------------------------------------------
bool PlaidApiClient::credentials_available() const noexcept {
    return !client_id_.empty() && !secret_.empty();
}

// ---------------------------------------------------------------------------
// base_url
// ---------------------------------------------------------------------------
std::string PlaidApiClient::base_url() const {
    if (env_ == "production") {
        return "https://production.plaid.com";
    } else if (env_ == "development") {
        return "https://development.plaid.com";
    }
    return "https://sandbox.plaid.com";
}

// ---------------------------------------------------------------------------
// build_auth_fields — returns raw JSON fragment for client_id and secret.
// GUARDRAIL: the caller must never log this string.
// ---------------------------------------------------------------------------
std::string PlaidApiClient::build_auth_fields() const {
    // Build manually to avoid a full json object for this inner fragment.
    // We'll use nlohmann but will only use it inside composed objects.
    return "";  // unused; composition happens in the full request body
}

// ---------------------------------------------------------------------------
// parse_plaid_date — "YYYY-MM-DD" → Unix seconds at midnight UTC
// ---------------------------------------------------------------------------
int64_t PlaidApiClient::parse_plaid_date(const std::string& date_str) {
    // Format: YYYY-MM-DD
    if (date_str.size() < 10) return 0;
    int year  = std::stoi(date_str.substr(0, 4));
    int month = std::stoi(date_str.substr(5, 2));
    int day   = std::stoi(date_str.substr(8, 2));

    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_isdst = -1;

#ifdef _WIN32
    // _mkgmtime on Windows for UTC
    time_t result = _mkgmtime(&t);
#else
    time_t result = timegm(&t);
#endif
    return static_cast<int64_t>(result);
}

// ---------------------------------------------------------------------------
// format_plaid_date — Unix seconds → "YYYY-MM-DD"
// ---------------------------------------------------------------------------
std::string PlaidApiClient::format_plaid_date(int64_t unix_secs) {
    time_t t = static_cast<time_t>(unix_secs);
    struct tm tm_buf{};
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

// ---------------------------------------------------------------------------
// sync_transactions — POST /transactions/sync
// ---------------------------------------------------------------------------
std::optional<TransactionList> PlaidApiClient::sync_transactions(
    std::string_view access_token,
    std::optional<std::string> cursor)
{
    if (!http_client_) return std::nullopt;

    // Build request body.
    // GUARDRAIL: access_token and credentials are placed in the request body
    //            but NEVER logged — even on failure.
    json body_obj = json::object();
    body_obj["client_id"]    = client_id_;
    body_obj["secret"]       = secret_;
    body_obj["access_token"] = std::string(access_token);
    if (cursor.has_value() && !cursor->empty()) {
        body_obj["cursor"] = *cursor;
    }
    // count: max per page (Plaid default is 100; request 500 for efficiency)
    body_obj["count"] = 500;

    std::string body_str = body_obj.dump();

    HttpRequest req;
    req.method = "POST";
    req.url    = base_url() + "/transactions/sync";
    req.headers["Content-Type"] = "application/json";
    req.headers["Plaid-Version"] = "2020-09-14";
    req.body   = body_str;
    req.timeout = std::chrono::milliseconds(30'000);

    // Zero the body_str after setting it on the request (the HttpRequest holds
    // a copy; our local variable can be zeroed).
    // Note: the copy in req.body still holds credentials — it will be destroyed
    // when req goes out of scope after send() returns.

    auto resp_opt = http_client_->send(req);

    // Zero local body_str now — credentials may be in it.
    std::fill(body_str.begin(), body_str.end(), '\0');
    // Zero the copy in req.body.
    if (req.body.has_value()) {
        std::fill(req.body->begin(), req.body->end(), '\0');
    }

    if (!resp_opt.has_value()) {
        // Transport error — DO NOT log access_token.
        std::cerr << "PlaidApiClient::sync_transactions: transport error\n";
        return std::nullopt;
    }

    if (resp_opt->status_code < 200 || resp_opt->status_code >= 300) {
        // Non-2xx — log status code only.
        std::cerr << "PlaidApiClient::sync_transactions: HTTP "
                  << resp_opt->status_code << "\n";
        return std::nullopt;
    }

    // Parse response.
    json resp_json;
    try {
        resp_json = json::parse(resp_opt->body);
    } catch (...) {
        std::cerr << "PlaidApiClient::sync_transactions: JSON parse failed\n";
        return std::nullopt;
    }

    TransactionList result;

    // added
    if (resp_json.contains("added") && resp_json["added"].is_array()) {
        for (const auto& tx : resp_json["added"]) {
            PlaidTransaction ptx;
            ptx.transaction_id = tx.value("transaction_id", "");
            ptx.account_id     = tx.value("account_id", "");
            // Plaid amounts are in dollars (positive = debit from perspective of user).
            // We convert to cents and invert sign so credit is positive.
            double amount_dollars = tx.value("amount", 0.0);
            ptx.amount_cents = static_cast<int64_t>(-amount_dollars * 100.0);
            // date field: "YYYY-MM-DD"
            std::string date_str = tx.value("date", "");
            ptx.date_unix = date_str.empty() ? 0 : parse_plaid_date(date_str);
            ptx.name      = tx.value("name", "");
            // category is an array in the Plaid API; we take the first element.
            if (tx.contains("category") && tx["category"].is_array()
                && !tx["category"].empty()) {
                ptx.category = tx["category"][0].get<std::string>();
            }
            result.added.push_back(std::move(ptx));
        }
    }

    // modified
    if (resp_json.contains("modified") && resp_json["modified"].is_array()) {
        for (const auto& tx : resp_json["modified"]) {
            PlaidTransaction ptx;
            ptx.transaction_id = tx.value("transaction_id", "");
            ptx.account_id     = tx.value("account_id", "");
            double amount_dollars = tx.value("amount", 0.0);
            ptx.amount_cents = static_cast<int64_t>(-amount_dollars * 100.0);
            std::string date_str = tx.value("date", "");
            ptx.date_unix = date_str.empty() ? 0 : parse_plaid_date(date_str);
            ptx.name      = tx.value("name", "");
            if (tx.contains("category") && tx["category"].is_array()
                && !tx["category"].empty()) {
                ptx.category = tx["category"][0].get<std::string>();
            }
            result.modified.push_back(std::move(ptx));
        }
    }

    // removed
    if (resp_json.contains("removed") && resp_json["removed"].is_array()) {
        for (const auto& tx : resp_json["removed"]) {
            std::string tid = tx.value("transaction_id", "");
            if (!tid.empty()) {
                result.removed_ids.push_back(std::move(tid));
            }
        }
    }

    result.next_cursor = resp_json.value("next_cursor", "");
    result.has_more    = resp_json.value("has_more", false);

    return result;
}

// ---------------------------------------------------------------------------
// fetch_transactions — POST /transactions/get
// ---------------------------------------------------------------------------
std::optional<TransactionList> PlaidApiClient::fetch_transactions(
    std::string_view access_token,
    std::string_view account_id,
    int64_t from_unix,
    int64_t to_unix)
{
    if (!http_client_) return std::nullopt;

    json body_obj = json::object();
    body_obj["client_id"]    = client_id_;
    body_obj["secret"]       = secret_;
    body_obj["access_token"] = std::string(access_token);
    body_obj["start_date"]   = format_plaid_date(from_unix);
    body_obj["end_date"]     = format_plaid_date(to_unix);

    if (!account_id.empty()) {
        body_obj["options"]["account_ids"] = json::array({std::string(account_id)});
    }

    std::string body_str = body_obj.dump();

    HttpRequest req;
    req.method = "POST";
    req.url    = base_url() + "/transactions/get";
    req.headers["Content-Type"] = "application/json";
    req.headers["Plaid-Version"] = "2020-09-14";
    req.body   = body_str;
    req.timeout = std::chrono::milliseconds(30'000);

    auto resp_opt = http_client_->send(req);

    // Zero credentials from local copies.
    std::fill(body_str.begin(), body_str.end(), '\0');
    if (req.body.has_value()) {
        std::fill(req.body->begin(), req.body->end(), '\0');
    }

    if (!resp_opt.has_value()) {
        std::cerr << "PlaidApiClient::fetch_transactions: transport error\n";
        return std::nullopt;
    }

    if (resp_opt->status_code < 200 || resp_opt->status_code >= 300) {
        std::cerr << "PlaidApiClient::fetch_transactions: HTTP "
                  << resp_opt->status_code << "\n";
        return std::nullopt;
    }

    json resp_json;
    try {
        resp_json = json::parse(resp_opt->body);
    } catch (...) {
        std::cerr << "PlaidApiClient::fetch_transactions: JSON parse failed\n";
        return std::nullopt;
    }

    TransactionList result;

    if (resp_json.contains("transactions") && resp_json["transactions"].is_array()) {
        for (const auto& tx : resp_json["transactions"]) {
            PlaidTransaction ptx;
            ptx.transaction_id = tx.value("transaction_id", "");
            ptx.account_id     = tx.value("account_id", "");
            double amount_dollars = tx.value("amount", 0.0);
            ptx.amount_cents = static_cast<int64_t>(-amount_dollars * 100.0);
            std::string date_str = tx.value("date", "");
            ptx.date_unix = date_str.empty() ? 0 : parse_plaid_date(date_str);
            ptx.name      = tx.value("name", "");
            if (tx.contains("category") && tx["category"].is_array()
                && !tx["category"].empty()) {
                ptx.category = tx["category"][0].get<std::string>();
            }
            result.added.push_back(std::move(ptx));
        }
    }

    result.has_more    = false;
    result.next_cursor = "";

    return result;
}

} // namespace tf::plaid
