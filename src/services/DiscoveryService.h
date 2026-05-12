/*!
 * @file DiscoveryService.h
 * @brief Shovel Intelligence engine for supplier discovery and spend velocity analysis.
 */

#pragma once

#include <string>
#include <map>
#include <vector>
#include <optional>

#include "../models/Transaction.h"
#include "../models/Account.h"
#include "../models/Category.h"
#include "../utils/Logger.h"

/*!
 * Legacy struct kept for backward source-compat with any caller that still
 * references it. New code should use SupplierRule.
 */
struct SupplierMapping {
    std::string description_keyword;  /*!< Raw text keyword to match */
    std::string ticker_symbol;        /*!< Stock ticker (e.g., AMZN) */
    std::string company_name;         /*!< Human-readable company name */
};

/*!
 * Match kinds supported in supplier_map.json.
 *   Contains: rule.match must appear as a substring of the (upper-cased)
 *             description.  This is the v0.1 behavior and the safe default
 *             for messy merchant strings.
 *   Prefix:   the (upper-cased) description must start with rule.match.
 */
enum class SupplierMatchKind {
    Contains,
    Prefix
};

/*!
 * A single supplier-recognition rule.  Loaded from data/supplier_map.json
 * (or the hardcoded fallback when the file is missing/unreadable).
 *
 * `ticker` may be empty for entities without a public listing
 * (e.g. USPS) — callers must tolerate an empty string here.
 */
struct SupplierRule {
    std::string match;                          /*!< Token to look for (upper-case) */
    SupplierMatchKind match_kind{SupplierMatchKind::Contains};
    std::string supplier;                       /*!< Human-readable supplier name */
    std::string ticker;                         /*!< Public ticker (may be empty) */
};

/*! Month-over-Month spend velocity for a category. */
struct VelocityResult {
    std::string category_id;          /*!< Category being measured */
    double current_month_spend;       /*!< Spend in current month */
    double previous_month_spend;      /*!< Spend in previous month */
    double percent_change;            /*!< MoM percentage change */
};

namespace tf::services {

/*!
 * Per-supplier spend aggregation used by both the Dashboard (top-N supplier
 * list) and the Shovel Score drill (full breakdown panel).  Promoted from
 * DashboardView.cpp's inline aggregation in Task v0.3-2 so the drill view
 * and the widget render off the same numbers.
 *
 * FIELDS
 *   ticker           Public-market ticker symbol ("NVDA", "AMZN"); the value
 *                    returned by DiscoveryService::mapToSupplier().  Empty
 *                    strings (e.g. USPS) are filtered out by the aggregator.
 *   total_spend      Lifetime absolute-value spend across every detected
 *                    expense transaction for this ticker.  Always >= 0.
 *   current_spend    Absolute-value spend in `current_month` ("YYYY-MM").
 *   previous_spend   Absolute-value spend in `previous_month` ("YYYY-MM").
 *   percent_change   Signed MoM percent change. ((cur - prev) / prev) * 100
 *                    when prev > 0; 100.0 when prev == 0 and cur > 0 (the
 *                    v0.1 first-month sentinel); 0.0 when both are zero.
 *
 * SORT ORDER
 *   aggregate_supplier_spend() returns the vector sorted DESCENDING by
 *   total_spend (largest shovel first) and TRUNCATED to the top 10.  This
 *   matches the v0.2 inline behavior in DashboardView::render() that
 *   Task v0.3-2 extracted verbatim.
 */
struct SupplierSpend {
    std::string ticker;
    double      total_spend;
    double      current_spend;
    double      previous_spend;
    double      percent_change;
};

}  // namespace tf::services

/*!
 * @class DiscoveryService
 * @brief Singleton service that cross-references transaction descriptions
 *        against known supplier keywords to identify infrastructure investments.
 *
 * Provides mapToSupplier() for single lookups and calculateVelocity() for
 * aggregated Month-over-Month spend analysis per category.
 */
class DiscoveryService {
public:
    static DiscoveryService& instance();

    /*!
     * @brief Maps a raw transaction description to a stock ticker.
     * @param description Bank statement description text.
     * @return Stock ticker symbol (e.g., "AMZN") or nullopt if no match.
     *
     * NOTE: returns the ticker even when it is the empty string (suppliers
     * with no public listing such as USPS).  Existing callers (DashboardView)
     * already tolerate empty strings.
     */
    std::optional<std::string> mapToSupplier(const std::string& description) const;

    /*!
     * @brief Returns the full SupplierRule for a description, if one matches.
     * @param description Bank statement description text.
     * @return The first matching SupplierRule, or nullopt.
     */
    std::optional<SupplierRule> getSupplierInfo(const std::string& description) const;

    /*!
     * @brief Calculates Month-over-Month spend velocity per category.
     * @param transactions All transactions to analyze.
     * @param categories Category definitions (used for display mapping).
     * @return Vector of VelocityResult with MoM changes.
     *
     * Uses std::chrono::system_clock to dynamically determine current/previous months.
     * Only expenses (negative amounts) are counted.
     */
    std::vector<VelocityResult> calculateVelocity(
        const std::vector<Transaction>& transactions,
        const std::vector<Category>& categories
    ) const;

    /*!
     * @brief Calculates MoM velocity using explicit month prefixes.
     *
     * Same semantics as calculateVelocity(...) but with caller-supplied
     * month prefixes (YYYY-MM), which makes the function deterministic
     * for unit tests.  Production code should continue to call the
     * single-arg overload.
     *
     * Returns one VelocityResult per category that has spend in either
     * month.  A category with zero spend in the previous month and
     * positive spend in the current month yields percent_change = 100.0
     * (the v0.1 "first-month" sentinel).  A category with zero spend in
     * both months is omitted.
     */
    std::vector<VelocityResult> calculateVelocityForMonths(
        const std::vector<Transaction>& transactions,
        const std::vector<Category>& categories,
        const std::string& current_month_prefix,   // YYYY-MM
        const std::string& previous_month_prefix   // YYYY-MM
    ) const;

    /*!
     * @brief Aggregate per-supplier expense spend across a transaction set.
     *
     * Extracted from the inline aggregation in DashboardView.cpp (commit
     * pre-c0fd7db lines 322-355) in Task v0.3-2 so the Dashboard and the
     * Shovel-Score drill view render off the same numbers.
     *
     * SEMANTICS (preserved byte-for-byte from the inline version)
     *   - Only negative-amount transactions count (expenses).  Income and
     *     refunds to a shovel-mapped ticker are filtered out.
     *   - tx.description is fed through mapToSupplier(); unrecognised
     *     descriptions are skipped silently.
     *   - For each ticker we sum three running totals:
     *       lifetime           = std::abs(tx.amount) over all matched tx
     *       current_month spend = sum where tx.date.substr(0,7) == current_month
     *       previous_month spend = sum where tx.date.substr(0,7) == previous_month
     *   - percent_change uses the same first-month sentinel as the rest of
     *     the codebase: 100.0 when prev == 0 and cur > 0; 0.0 when both
     *     are zero; otherwise ((cur - prev) / prev) * 100.
     *
     * SORT
     *   The returned vector is sorted DESCENDING by total_spend (largest
     *   shovel first), with ties broken alphabetically by ticker for
     *   deterministic ordering.  No truncation: callers that want a top-N
     *   slice (e.g. the Shovel-Score drill view's "top-10 inputs" panel)
     *   take the first N entries themselves.  The Dashboard widget keeps
     *   receiving the full vector, preserving v0.2 visual byte-for-byte.
     *
     * @param transactions    The full transaction set (DataStore::transactions).
     * @param current_month   "YYYY-MM" bucket for the current-month column.
     * @param previous_month  "YYYY-MM" bucket for the previous-month column.
     * @return Up to 10 SupplierSpend rows, sorted desc by total_spend.
     */
    std::vector<tf::services::SupplierSpend> aggregate_supplier_spend(
        const std::vector<Transaction>& transactions,
        const std::string& current_month,
        const std::string& previous_month
    ) const;

    /*!
     * @brief Replace the in-memory rules from a JSON file on disk.
     * @param path Filesystem path to a supplier_map.json file.
     * @return true on success, false on missing/malformed file.
     *
     * On failure the existing rules are NOT modified.  Callers can treat
     * this as a best-effort refresh.
     */
    bool load_from_json(const std::string& path);

    /*!
     * @brief Returns the current rule set as a JSON-serialisable structure.
     *
     * Used by the server's GET /supplier-map endpoint.  Output schema:
     *   { "version": 1,
     *     "rules": [
     *       { "match": "...", "match_kind": "prefix|contains",
     *         "supplier": "...", "ticker": "..." }, ...
     *     ]
     *   }
     */
    std::string get_canonical_mapping_json() const;

    /*!
     * @brief Direct access to the parsed rules (for tests and the server endpoint).
     */
    const std::vector<SupplierRule>& rules() const { return rules_; }

    /*! Reset rules to the built-in hardcoded fallback. */
    void initializeSupplierMap();

private:
    DiscoveryService();
    std::vector<SupplierRule> rules_;
};
