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
