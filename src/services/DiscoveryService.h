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

/*! Mapping from description keyword to supplier info. */
struct SupplierMapping {
    std::string description_keyword;  /*!< Raw text keyword to match */
    std::string ticker_symbol;        /*!< Stock ticker (e.g., AMZN) */
    std::string company_name;         /*!< Human-readable company name */
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
     */
    std::optional<std::string> mapToSupplier(const std::string& description) const;

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

    void initializeSupplierMap();

private:
    DiscoveryService() { initializeSupplierMap(); }
    std::map<std::string, SupplierMapping> supplier_map_;
};