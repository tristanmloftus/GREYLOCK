// ---------------------------------------------------------------------------
// DashboardView.cpp — Phase 5 implementation of the default TUI view.
// ---------------------------------------------------------------------------
// This file wires the five widgets under src/views/widgets/ into a single
// FTXUI Element returned each frame.  It is pure composition: every data
// source is owned elsewhere (DataStore for accounts/transactions,
// DiscoveryService for ticker mapping) and this file only aggregates and
// formats.
//
// DATA SOURCING STRATEGY
//   1. Server-side (server/): a SQLCipher-encrypted SQLite database holds
//      the authoritative accounts, transactions, and categories tables.
//   2. BackendClient (HTTP): the TUI process pulls those tables on sync
//      and writes them into DataStore (the TUI-side in-memory cache).
//   3. DashboardView (this file): re-reads DataStore on every render(),
//      computes the per-panel aggregates inline, and hands the resulting
//      POD vectors / scalars to each widget's renderer.
//
// PER-PANEL WIRING (summary; per-widget blocks below have the details)
//   ui_net_worth          <- DataStore::accounts() summed by AccountType
//                            (Checking / Savings / CreditCard / Investment).
//                            Restricted to entity_id_ when non-empty.
//
//   ui_category_trends    <- DataStore::transactions() filtered to two
//                            adjacent months, aggregated per category_id.
//                            percent_change = MoM (current vs. previous).
//                            Hard-coded display names + ASCII emoji follow
//                            the v0.1 DashboardView convention.
//
//   ui_shovel_intelligence<- DiscoveryService::mapToSupplier() over every
//                            transaction description.  Per-ticker MoM
//                            percent_change is computed inline.
//
//   ui_shovel_score       <- compute_shovel_score() over the MoM velocity
//                            of each discovered ticker (see KNOWN STOP-GAP
//                            below).
//
//   ui_sync_status        <- DataStore-derived fallback: per-institution
//                            newest transaction date + connection inferred
//                            from whether the institution's accounts have
//                            any transactions in DataStore.  Will switch
//                            to BackendClient::sync_status() when that
//                            method ships.
//
// KNOWN STOP-GAP — SHOVEL SCORE
//   compute_shovel_score() is a placeholder.  It computes the median of
//   |MoM percent_change| over the top-10 shovel suppliers (by absolute
//   spend), then clamps to [0, 100].  This is NOT a real model — see
//   compute_shovel_score() below for what a real replacement should look
//   like.  Tracked as TODO(shovel-score).
//
// SEE ALSO
//   V0_2_PLAN.md § Phase 5 — Dashboard live wiring.
//   src/views/widgets/* for the individual panel renderers.
//   src/services/DiscoveryService.h for the shovel-supplier mapping.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// month_offset
// ---------------------------------------------------------------------------
// Subtract `months` from a "YYYY-MM" string and return the new "YYYY-MM".
// Used by render() to derive the "previous month" key from the caller-
// supplied current_month so we can compute MoM deltas without pulling in
// <chrono>'s year_month_day (which would require C++20 calendar headers
// that not all of the project's translation units currently include).
//
// Tolerates malformed input by returning the input unchanged when shorter
// than 7 chars — render() will then bucket no transactions into the prev
// month, which surfaces in the UI as "first month of data" deltas.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// sum_expense_by_category_in_month
// ---------------------------------------------------------------------------
// Sum the absolute value of negative-amount transactions for a given
// category_id within a single "YYYY-MM" bucket.  Income (tx.amount >= 0)
// is excluded — this function intentionally surfaces expense magnitudes
// only, which is what the Category Trends panel wants to display.
// Returned value is in the same currency unit as Transaction::amount
// (dollars in v0.2; cents conversion is a v0.3 follow-up).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// compute_shovel_score    [v0.2 STOP-GAP — TODO(shovel-score)]
// ---------------------------------------------------------------------------
// FORMULA (as implemented):
//   1. Take the input vector of per-ticker |MoM percent_change| values.
//   2. Sort ascending.
//   3. Keep only the top-N (N = min(10, size)) by velocity.
//   4. Re-sort that subset and return the median, clamped to [0, 100].
//
// WHY THIS IS A STOP-GAP:
//   The current formula has obvious flaws:
//     - It ignores dollar concentration entirely (a $5 ticker with 100%
//       MoM growth weighs the same as a $50,000 one).
//     - "Top-N by velocity" is a poor proxy for "are you actually
//       building an AI-infrastructure spend pattern?"
//     - The clamp at 100 is arbitrary — real MoM can exceed 100% easily,
//       and capping discards signal.
//   It exists so the Dashboard has a number to display in v0.2 demos.
//
// WHAT A REAL SCORER SHOULD CONSIDER:
//   - Number of distinct shovel tickers detected (breadth).
//   - Total $ concentration in shovel tickers vs. all expense (depth).
//   - Per-ticker MoM growth dispersion (consistent climb vs. one-time
//     spike).
//   - Recency-weighted contribution so historical noise decays.
//   - Calibration against a labeled "powerhouse / early-adopter / etc."
//     bucket — currently the label thresholds in ui_shovel_score.cpp
//     are unanchored magic numbers.
//
// REPLACEMENT TRIGGER:
//   When the discovery-engineer ships a real scoring API on
//   DiscoveryService, replace this function with a thin call site.
//   Tracked: TODO(shovel-score).
//
// CALLERS:
//   DashboardView::render() — only.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// DashboardView::render
// ---------------------------------------------------------------------------
// Called once per frame by the TUI driver.  Aggregates DataStore on each
// call (no memoization) and composes the five panels into the two-row
// responsive grid.  No allocation beyond what the per-panel POD vectors
// require; the FTXUI Element graph is created fresh each frame.
// ---------------------------------------------------------------------------
ftxui::Element DashboardView::render(const std::string& current_month) {
    using namespace ftxui;

    // ======================================================================
    // PANEL 1: ui_net_worth
    // ----------------------------------------------------------------------
    // Shows: per-account-type running balances and the total net worth.
    // Data: DataStore::accounts() — either the global view or restricted
    //       to entity_id_ when set (e.g. "rory" vs "pcc").
    // Transform: a single pass over accounts grouping by AccountType.
    //            Net worth itself is taken from DataStore's authoritative
    //            getter (which already handles the sign of liabilities).
    // Caveats: balances are taken at face value — no historical reprice
    //          of investment positions.
    // ======================================================================
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

    // ======================================================================
    // PANEL 2: ui_category_trends
    // ----------------------------------------------------------------------
    // Shows: the top spending categories for current_month with MoM deltas.
    // Data: DataStore::transactions (read directly via the public member,
    //       not a getter — the v0.1 convention DataStore still exposes).
    // Transform: for each hard-coded category_id, sum_expense_by_category_
    //            in_month() at current_month and prev_month, then compute
    //            percent_change = ((cur - prev) / prev) * 100, with the
    //            special-case "no prev → 100% growth or 0%" branch below.
    // Caveats:
    //   - The category set is hard-coded here (not pulled from the
    //     categories table) to match v0.1 demo behavior.  When the
    //     categories endpoint stabilizes, swap this list for a server
    //     pull.  TODO(v0.3).
    //   - Categories with zero spend in both months are dropped so the
    //     panel doesn't show pure-zero rows.
    // ======================================================================
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
        // Edge case: no prior-month baseline.  Show 100% if there is any
        // current spend (treated as "new this month") and 0% otherwise.
        ct.percent_change = (prev > 0.0) ? ((cur - prev) / prev) * 100.0 : (cur > 0.0 ? 100.0 : 0.0);
        category_trends.push_back(ct);
    }
    Element category_trends_panel =
        CategorySpendingTrendsRenderer(category_trends, /*max_items*/5);

    // ======================================================================
    // PANELS 3 + 4: ui_shovel_intelligence + ui_shovel_score
    // ----------------------------------------------------------------------
    // Shows: discovered AI-infrastructure suppliers ("shovels") with
    //        per-ticker spend + MoM growth, plus the composite Shovel Score.
    // Data: DataStore::transactions, mapped to a ticker via
    //       DiscoveryService::instance().mapToSupplier(tx.description).
    // Transform: three running totals per ticker (lifetime, current-month,
    //            previous-month).  percent_change per ticker uses the same
    //            ((cur - prev) / prev) * 100 formula as category_trends,
    //            with the same first-month edge-case branch.
    //            Suppliers are sorted desc by lifetime spend for display.
    //            shovel_score_value comes from compute_shovel_score(), the
    //            documented v0.2 stop-gap.
    // Caveats:
    //   - DiscoveryService is a singleton today; tests stub it via a
    //     compile-time hook.  Production swap to DI is a v0.3 follow-up.
    //   - Only negative-amount transactions count (expenses); refunds and
    //     deposits to a shovel ticker (rare) are filtered out.
    //   - The MoM velocity vector passed to compute_shovel_score() uses
    //     std::abs(pct) — direction is intentionally discarded because
    //     the current scorer treats "growth" symmetrically with "shrink".
    //     A real model should NOT do this.  TODO(shovel-score).
    // ======================================================================
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
    // Sort by absolute spend desc for display (largest shovels first).
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

    // ======================================================================
    // PANEL 5: ui_sync_status
    // ----------------------------------------------------------------------
    // Shows: per-institution connection state and last-sync timestamp.
    // Data: DataStore::accounts + DataStore::transactions.
    // Transform: derive a per-institution newest-tx date by scanning all
    //            transactions and account_id -> institution lookup.  An
    //            institution is "connected" iff at least one of its
    //            accounts has any transactions in DataStore.
    // Caveats:
    //   - This is a FALLBACK.  When BackendClient adds sync_status() the
    //     view should switch to that authoritative source instead.
    //     Today, an institution with stale data but no errors will still
    //     show as "connected".
    //   - last_sync here is really "newest transaction date", not "last
    //     time we polled the server".  Same field name, slightly
    //     different semantics — preserved for visual continuity.
    // ======================================================================
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
        // Lexicographic compare on "YYYY-MM-DD" strings is equivalent to
        // chronological compare — relied on intentionally.
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

    // ======================================================================
    // COMPOSE: two-row responsive grid.
    //   Top row:    [ NetWorth ][ ShovelScore ][ SyncStatus ]
    //   Bottom row: [ ShovelIntelligence ][ CategoryTrends ]
    // Each panel uses `| flex` so the row divides available width
    // evenly; `vbox(...) | flex` lets the whole dashboard fill the
    // terminal's vertical extent.
    // ======================================================================
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
