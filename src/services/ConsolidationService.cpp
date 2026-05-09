#include "ConsolidationService.h"
#include "../utils/Logger.h"
#include <sstream>
#include <iomanip>
#include <map>
#include <algorithm>

ConsolidationService& ConsolidationService::instance() {
    static ConsolidationService instance;
    return instance;
}

std::string ConsolidationService::compute_transaction_hash(const PlaidTransaction& tx) const {
    std::ostringstream oss;
    oss << tx.transaction_id << "|" << std::fixed << std::setprecision(2) << tx.amount << "|" << tx.date;
    return oss.str();
}

std::string ConsolidationService::compute_transaction_hash(const Transaction& tx) const {
    std::ostringstream oss;
    oss << tx.plaid_transaction_id << "|" << std::fixed << std::setprecision(2) << tx.amount << "|" << tx.date;
    return oss.str();
}

std::vector<Transaction> ConsolidationService::normalize_transactions(
    const std::vector<PlaidTransaction>& plaid_txs,
    const std::string& account_id
) const {
    std::vector<Transaction> result;
    result.reserve(plaid_txs.size());

    for (const auto& pt : plaid_txs) {
        Transaction tx;
        tx.id = "tx_" + pt.transaction_id;
        tx.plaid_transaction_id = pt.transaction_id;
        tx.account_id = account_id;
        tx.date = pt.date;
        tx.amount = -pt.amount;
        tx.description = pt.description;
        tx.pending = pt.pending;

        if (pt.category.empty()) {
            tx.category_id = "cat_other_expense";
        } else {
            tx.category_id = map_category(pt.category);
        }

        result.push_back(tx);
    }

    Logger::instance().info("ConsolidationService: Normalized " + std::to_string(result.size()) + " transactions");
    return result;
}

std::string ConsolidationService::map_category(const std::string& plaid_category) const {
    std::string lower = plaid_category;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("food") != std::string::npos || lower.find("restaurant") != std::string::npos) {
        return "cat_food";
    }
    if (lower.find("travel") != std::string::npos || lower.find("transport") != std::string::npos) {
        return "cat_transport";
    }
    if (lower.find("shop") != std::string::npos || lower.find("merchandise") != std::string::npos) {
        return "cat_shopping";
    }
    if (lower.find("util") != std::string::npos || lower.find("electric") != std::string::npos) {
        return "cat_utilities";
    }
    if (lower.find("enter") != std::string::npos || lower.find("recreation") != std::string::npos) {
        return "cat_entertainment";
    }
    if (lower.find("health") != std::string::npos || lower.find("medical") != std::string::npos) {
        return "cat_health";
    }
    if (lower.find("transfer") != std::string::npos) {
        return "cat_transfer";
    }
    if (lower.find("deposit") != std::string::npos || lower.find("payroll") != std::string::npos) {
        return "cat_salary";
    }

    return "cat_other_expense";
}

DeduplicationResult ConsolidationService::merge_transactions(
    std::vector<Transaction>& existing,
    const std::vector<Transaction>& incoming
) const {
    DeduplicationResult result = {0, 0, 0};

    std::set<std::string> existing_hashes;
    for (const auto& tx : existing) {
        if (!tx.plaid_transaction_id.empty()) {
            existing_hashes.insert(compute_transaction_hash(tx));
        }
    }

    for (const auto& incoming_tx : incoming) {
        std::string hash = compute_transaction_hash(incoming_tx);

        if (existing_hashes.find(hash) != existing_hashes.end()) {
            result.duplicate_transactions++;
            continue;
        }

        auto it = std::find_if(existing.begin(), existing.end(),
            [&incoming_tx](const Transaction& tx) {
                return tx.plaid_transaction_id == incoming_tx.plaid_transaction_id &&
                       !tx.plaid_transaction_id.empty();
            });

        if (it != existing.end()) {
            if (it->notes.empty() && !incoming_tx.notes.empty()) {
                it->notes = incoming_tx.notes;
            }
            if (it->category_id.empty() && !incoming_tx.category_id.empty()) {
                it->category_id = incoming_tx.category_id;
            }
            it->pending = incoming_tx.pending;
            result.updated_transactions++;
        } else {
            existing.push_back(incoming_tx);
            existing_hashes.insert(hash);
            result.new_transactions++;
        }
    }

    Logger::instance().info("ConsolidationService: Merge complete - " +
        std::to_string(result.new_transactions) + " new, " +
        std::to_string(result.duplicate_transactions) + " duplicates, " +
        std::to_string(result.updated_transactions) + " updated");

    return result;
}

std::vector<Account> ConsolidationService::merge_accounts(
    const std::vector<Account>& existing,
    const std::vector<PlaidAccount>& plaid_accounts,
    const std::string& entity_id
) const {
    std::map<std::string, Account> accounts_by_id;

    for (const auto& acc : existing) {
        accounts_by_id[acc.id] = acc;
    }

    for (const auto& pa : plaid_accounts) {
        std::string new_id = "acc_plaid_" + pa.account_id;
        auto it = accounts_by_id.find(new_id);

        if (it != accounts_by_id.end()) {
            it->second.balance = pa.balance;
            it->second.name = pa.name;
            it->second.is_active = true;
        } else {
            Account acc;
            acc.id = new_id;
            acc.name = pa.name;
            acc.entity_id = entity_id;
            acc.balance = pa.balance;
            acc.plaid_item_id = pa.account_id;
            acc.is_active = true;

            if (pa.type == "depository") {
                acc.type = (pa.subtype == "savings") ? AccountType::Savings : AccountType::Checking;
            } else if (pa.type == "credit") {
                acc.type = AccountType::CreditCard;
            } else if (pa.type == "investment") {
                acc.type = AccountType::Investment;
            } else {
                acc.type = AccountType::Other;
            }

            accounts_by_id[new_id] = acc;
        }
    }

    std::vector<Account> result;
    for (const auto& [id, acc] : accounts_by_id) {
        result.push_back(acc);
    }

    Logger::instance().info("ConsolidationService: Merged " + std::to_string(result.size()) + " accounts");
    return result;
}

TotalLiquidity ConsolidationService::calculate_liquidity(const std::vector<Account>& accounts) const {
    TotalLiquidity liq = {0, 0, 0, 0, 0, 0};

    for (const auto& acc : accounts) {
        if (!acc.is_active) continue;

        switch (acc.type) {
            case AccountType::Checking:
                liq.checking += acc.balance;
                liq.total += acc.balance;
                liq.net_worth += acc.balance;
                break;
            case AccountType::Savings:
                liq.savings += acc.balance;
                liq.total += acc.balance;
                liq.net_worth += acc.balance;
                break;
            case AccountType::CreditCard:
                liq.credit += acc.balance;
                liq.total += acc.balance;
                liq.net_worth += acc.balance;
                break;
            case AccountType::Investment:
                liq.investment += acc.balance;
                liq.total += acc.balance;
                liq.net_worth += acc.balance;
                break;
            default:
                break;
        }
    }

    return liq;
}