// DashboardView.cpp — Phase 5 live wiring.
//
// Each of the five required exit panels is sourced as follows:
//
//   ui_net_worth          <- DataStore::accounts() summed by AccountType
//                            (Checking/Savings/CreditCard/Investment).
//                            For non-empty entity_id_, restricts to that entity.
//
//   ui_category_trends    <- DataStore::transactions() filtered to the last
//                            3 months, aggregated per category_id.  Hard-coded
//                            display names + emoji follow the v0.1 DashboardView
//                            convention (category_id -> human name).
//                            percent_change = MoM (current vs. previous month).
//
//   ui_shovel_intelligence<- DiscoveryService::mapToSupplier() over every
//                            transaction description.  Tickers are grouped;
//                            per-ticker MoM percent_change is computed.
//
//   ui_shovel_score       <- Median MoM velocity across the top-10 shovel
//                            suppliers (by absolute spend), clamped to [0, 100].
//                            See compute_shovel_score() below for the exact
//                            formula; this is the documented v0.2 stop-gap
//                            until a real scoring model lands.
//
//   ui_sync_status        <- BackendClient does not currently expose
//                            sync_status(); fall back to DataStore-derived
//                            counts: per-institution last-tx date + connection
//                            inferred from whether the account has any
//                            transactions in DataStore.

#include "DashboardView.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../models/Account.h"
#include "../models/DataStore.h"
#include "../models/Transaction.h"
#include "../services/DiscoveryService.h"
#include "ViewCommon.h"
#include "widgets/ui_category_trends.h"
#include "widgets/ui_net_worth.h"
#include "widgets/ui_shovel_intelligence.h"
#include "widgets/ui_shovel_score.h"
#include "widgets/ui_sync_status.h"

namespace {

// Subtract `months` from a "YYYY-MM" string; returns the new "YYYY-MM".
std::string month_offset(const std::string& yyyymm, int months) {
    if (yyyymm.size() < 7) return yyyymm;
    int year = std::stoi(yyyymm.substr(0, 4));
    int mon = std::stoi(yyyymm.substr(5, 2));
    int absolute = year * 12 + (mon - 1) - months;
    int ny = absolute / 12;
    int nm = absolute % 12 + 1;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", ny, nm);
    return std::string(buf);
}

double sum_expense_by_category_in_month(
    const std::vector<Transaction>& txs,
    const std::string& category_id,
    const std::string& yyyymm
) {
    double total = 0.0;
    for (const auto& tx : txs) {
        if (tx.category_id != category_id) continue;
        if (tx.amount >= 0) continue;
        if (tx.date.substr(0, 7) != yyyymm) continue;
        total += std::abs(tx.amount);
    }
    return total;
}

// Compute the v0.2 stop-gap Shovel Score:
//   median MoM percent_change (in absolute value) across top-10 suppliers
//   by spend, then clamp to [0, 100].  Documented in DashboardView.cpp
//   header comment.  Replace with a real model when the discovery-engineer
//   exposes a scoring API.
double compute_shovel_score(std::vector<double> velocities) {
    if (velocities.empty()) return 0.0;
    std::sort(velocities.begin(), velocities.end());
    const size_t n = std::min<size_t>(velocities.size(), 10);
    std::vector<double> top(velocities.end() - static_cast<long>(n), velocities.end());
    std::sort(top.begin(), top.end());
    const double median = top[top.size() / 2];
    return std::clamp(median, 0.0, 100.0);
}

// FTXUI types we use at file scope.
using ftxui::Element;
using ftxui::Elements;
using ftxui::filler;
using ftxui::hbox;
using ftxui::text;
using ftxui::vbox;

}  // namespace

DashboardView::DashboardView(DataStore& data_store)
    : data_store_(data_store) {}

ftxui::Element DashboardView::render(const std::string& current_month) {
    using namespace ftxui;

    // ------------------------------------------------------------------
    // ui_net_worth panel <- DataStore::accounts() summed by AccountType.
    // ------------------------------------------------------------------
    double checking = 0, savings = 0, credit = 0, investment = 0;
    double net_worth = 0;

    if (!entity_id_.empty()) {
        net_worth = data_store_.get_total_net_worth_for_entity(entity_id_);
        for (auto* acc : data_store_.get_accounts_for_entity(entity_id_)) {
            switch (acc->type) {
                case AccountType::Checking:   checking   += acc->balance; break;
                case AccountType::Savings:    savings    += acc->balance; break;
                case AccountType::CreditCard: credit     += acc->balance; break;
                case AccountType::Investment: investment += acc->balance; break;
                default: break;
            }
        }
    } else {
        net_worth  = data_store_.get_total_net_worth();
        checking   = data_store_.get_total_balance_by_type(AccountType::Checking);
        savings    = data_store_.get_total_balance_by_type(AccountType::Savings);
        credit     = data_store_.get_total_balance_by_type(AccountType::CreditCard);
        investment = data_store_.get_total_balance_by_type(AccountType::Investment);
    }

    Element net_worth_panel =
        NetWorthBreakdownRenderer(checking, savings, credit, investment, net_worth);

    // ------------------------------------------------------------------
    // ui_category_trends panel <- DataStore::transactions() over last 3 months.
    // ------------------------------------------------------------------
    const std::vector<std::pair<std::string, std::string>> cat_info = {
        {"Food & Dining",  "[food]"},
        {"Transportation", "[trsp]"},
        {"Shopping",       "[shop]"},
        {"Entertainment",  "[ent ]"},
        {"Utilities",      "[util]"},
    };
    const std::vector<std::string> cat_ids = {
        "cat_food", "cat_transport", "cat_shopping", "cat_entertainment", "cat_utilities"
    };

    const std::string prev_month = month_offset(current_month, 1);

    std::vector<tf::widgets::CategoryTrend> category_trends;
    for (size_t i = 0; i < cat_info.size(); ++i) {
        const double cur  = sum_expense_by_category_in_month(data_store_.transactions, cat_ids[i], current_month);
        const double prev = sum_expense_by_category_in_month(data_store_.transactions, cat_ids[i], prev_month);
        if (cur <= 0.0 && prev <= 0.0) continue;
        tf::widgets::CategoryTrend ct;
        ct.category_name  = cat_info[i].first;
        ct.emoji          = cat_info[i].second;
        ct.current_spend  = cur;
        ct.percent_change = (prev > 0.0) ? ((cur - prev) / prev) * 100.0 : (cur > 0.0 ? 100.0 : 0.0);
        category_trends.push_back(ct);
    }
    Element category_trends_panel =
        CategorySpendingTrendsRenderer(category_trends, /*max_items*/5);

    // ------------------------------------------------------------------
    // ui_shovel_intelligence + ui_shovel_score
    //   Source: DiscoveryService::mapToSupplier() over all transactions.
    //   percent_change per ticker = ((current_month - prev_month) / prev_month) * 100.
    // ------------------------------------------------------------------
    auto& discovery = DiscoveryService::instance();

    std::map<std::string, double> ticker_total;       // lifetime absolute spend
    std::map<std::string, double> ticker_cur_month;   // current-month absolute spend
    std::map<std::string, double> ticker_prev_month;  // prev-month absolute spend

    for (const auto& tx : data_store_.transactions) {
        auto ticker = discovery.mapToSupplier(tx.description);
        if (!ticker) continue;
        if (tx.amount >= 0) continue;  // expenses only
        const double abs_amt = std::abs(tx.amount);
        ticker_total[*ticker] += abs_amt;
        const std::string ym = tx.date.substr(0, 7);
        if (ym == current_month) ticker_cur_month[*ticker]  += abs_amt;
        else if (ym == prev_month) ticker_prev_month[*ticker] += abs_amt;
    }

    std::vector<tf::widgets::SupplierSpend> suppliers;
    std::vector<double> mom_velocities;
    for (const auto& [ticker, total] : ticker_total) {
        const double cur  = ticker_cur_month[ticker];
        const double prev = ticker_prev_month[ticker];
        const double pct = (prev > 0.0) ? ((cur - prev) / prev) * 100.0
                                        : (cur > 0.0 ? 100.0 : 0.0);
        tf::widgets::SupplierSpend ss;
        ss.ticker = ticker;
        ss.amount = total;
        ss.percent_change = pct;
        suppliers.push_back(ss);
        mom_velocities.push_back(std::abs(pct));
    }
    // Sort by absolute spend desc for display.
    std::sort(suppliers.begin(), suppliers.end(),
              [](const auto& a, const auto& b) { return a.amount > b.amount; });

    Element shovel_intel_panel = ShovelIntelligenceRenderer(suppliers);

    double total_shovel_spend = 0.0;
    for (const auto& [_, t] : ticker_total) total_shovel_spend += t;
    const double shovel_score_value = compute_shovel_score(mom_velocities);
    Element shovel_score_panel =
        ShovelScoreRenderer(shovel_score_value,
                            static_cast<int>(suppliers.size()),
                            total_shovel_spend);

    // ------------------------------------------------------------------
    // ui_sync_status <- DataStore-derived fallback.
    //   For each institution, find the newest transaction date.  Treat the
    //   account as "connected" if it has any transactions in DataStore.
    // ------------------------------------------------------------------
    std::map<std::string, std::string> inst_to_newest_date;
    std::map<std::string, bool>        inst_connected;
    std::map<std::string, std::string> account_to_inst;
    for (const auto& acc : data_store_.accounts) {
        if (acc.institution.empty()) continue;
        account_to_inst[acc.id] = acc.institution;
        // Default to not-connected; flip below if we see transactions.
        if (inst_connected.find(acc.institution) == inst_connected.end()) {
            inst_connected[acc.institution] = false;
        }
    }
    for (const auto& tx : data_store_.transactions) {
        auto it = account_to_inst.find(tx.account_id);
        if (it == account_to_inst.end()) continue;
        const std::string& inst = it->second;
        inst_connected[inst] = true;
        auto& newest = inst_to_newest_date[inst];
        if (tx.date > newest) newest = tx.date;
    }
    std::vector<tf::widgets::SyncStatus> sync_statuses;
    for (const auto& [inst, _] : inst_connected) {
        tf::widgets::SyncStatus s;
        s.institution = inst;
        s.connected   = inst_connected[inst];
        s.last_sync   = inst_to_newest_date[inst];
        sync_statuses.push_back(s);
    }
    std::sort(sync_statuses.begin(), sync_statuses.end(),
              [](const auto& a, const auto& b) { return a.institution < b.institution; });

    Element sync_status_panel = SyncStatusIndicatorRenderer(sync_statuses);

    // ------------------------------------------------------------------
    // Compose: two-column responsive grid using hbox + filler.
    //   Top row:    [ NetWorth ][ ShovelScore ][ SyncStatus ]
    //   Bottom row: [ ShovelIntelligence ][ CategoryTrends ]
    // ------------------------------------------------------------------
    return vbox({
        hbox({
            net_worth_panel | flex,
            shovel_score_panel | flex,
            sync_status_panel | flex,
        }),
        hbox({
            shovel_intel_panel | flex,
            category_trends_panel | flex,
        }),
    }) | flex;
}
