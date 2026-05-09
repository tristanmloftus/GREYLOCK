/*!
 * @file ConsolidationService.h
 * @brief Deduplication and multi-account merge engine for Plaid bank data.
 */
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <set>

#include "../models/Transaction.h"
#include "../models/Account.h"
#include "../services/PlaidService.h"

/*! Result summary of a merge operation. */
struct DeduplicationResult {
    size_t new_transactions;      /*!< Transactions added that were not seen before */
    size_t duplicate_transactions; /*!< Transactions skipped as exact duplicates */
    size_t updated_transactions;   /*!< Existing transactions updated with newer data */
};

/*! Aggregated liquidity breakdown across account types. */
struct TotalLiquidity {
    double checking;   /*!< Sum of all checking account balances */
    double savings;    /*!< Sum of all savings account balances */
    double credit;     /*!< Sum of all credit account balances */
    double investment; /*!< Sum of all investment account balances */
    double total;      /*!< Sum of all accounts */
    double net_worth;  /*!< Net worth (total with credit adjusted) */
};

/*!
 * @class ConsolidationService
 * @brief Singleton that merges Plaid API responses into the DataStore
 *        without creating duplicate entries.
 *
 * Uses a hash-based deduplication engine (transaction_id + amount + date)
 * and preserves user-added notes during bank data refresh.
 */
class ConsolidationService {
public:
    static ConsolidationService& instance();

    /*!
     * @brief Computes a unique hash for a Plaid transaction.
     * @param tx Plaid transaction struct.
     * @return Hash string of "transaction_id|amount|date".
     */
    std::string compute_transaction_hash(const PlaidTransaction& tx) const;

    /*!
     * @brief Computes a unique hash for an internal Transaction.
     * @param tx Internal transaction struct.
     * @return Hash string of "plaid_transaction_id|amount|date".
     */
    std::string compute_transaction_hash(const Transaction& tx) const;

    /*!
     * @brief Converts Plaid transaction format to internal Transaction format.
     * @param plaid_txs Raw Plaid API transactions.
     * @param account_id Internal account ID to assign.
     * @return Vector of normalized Transaction objects.
     */
    std::vector<Transaction> normalize_transactions(
        const std::vector<PlaidTransaction>& plaid_txs,
        const std::string& account_id
    ) const;

    /*!
     * @brief Merges incoming transactions into existing list, deduplicating by hash.
     * @param existing Reference to the existing transaction vector (modified in place).
     * @param incoming New transactions to merge.
     * @return DeduplicationResult with counts.
     */
    DeduplicationResult merge_transactions(
        std::vector<Transaction>& existing,
        const std::vector<Transaction>& incoming
    ) const;

    /*!
     * @brief Merges Plaid accounts into existing account list.
     * @param existing Existing account vector.
     * @param plaid_accounts Accounts from Plaid API.
     * @param entity_id Entity to associate with new accounts.
     * @return Merged account vector.
     */
    std::vector<Account> merge_accounts(
        const std::vector<Account>& existing,
        const std::vector<PlaidAccount>& plaid_accounts,
        const std::string& entity_id
    ) const;

    /*!
     * @brief Calculates total liquidity across all account types.
     * @param accounts Account vector to analyze.
     * @return TotalLiquidity struct with breakdown.
     */
    TotalLiquidity calculate_liquidity(const std::vector<Account>& accounts) const;

    void set_existing_transactions(const std::vector<Transaction>& transactions);
    std::string get_last_error() const { return last_error_; }

private:
    ConsolidationService() = default;
    std::set<std::string> existing_hashes_;
    std::string last_error_;

    /*!
     * @brief Maps a Plaid category string to an internal category ID.
     * @param plaid_category Raw category string from Plaid's API.
     * @return Internal category ID (e.g., "cat_food", "cat_transport").
     */
    std::string map_category(const std::string& plaid_category) const;
};