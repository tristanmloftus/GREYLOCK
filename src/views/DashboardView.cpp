// ---------------------------------------------------------------------------
// DashboardView.cpp — composes the Dashboard widgets into one FTXUI Element.
// ---------------------------------------------------------------------------
// Pure composition: data is owned elsewhere (DataStore is the TUI-side
// in-memory cache, populated by BackendClient from the SQLCipher server DB).
// This file re-reads DataStore on every render(), computes the per-panel
// aggregates inline, and hands POD vectors/scalars to each widget renderer.
//
// CURRENT PANELS (post 2026-05-16 shovel removal — 2x2 grid):
//   ui_net_worth        <- DataStore::accounts summed by AccountType.
//                           Restricted to entity_id_ when set.
//   ui_sync_status      <- per-institution newest transaction date +
//                           connection inferred from whether the
//                           institution's accounts have any transactions
//                           in DataStore. Will switch to a real backend
//                           sync_status() when that ships.
//   ui_category_trends  <- DataStore::transactions filtered to two adjacent
//                           months, aggregated per category_id.
//                           percent_change = MoM (current vs. previous).
//
// Replacement widgets for the (row 1, col 1) empty cell are TBD per
// greylock-kickoff.md §3.3.
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
#include "ViewCommon.h"
#include "widgets/ui_category_trends.h"
#include "widgets/ui_net_worth.h"
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
ftxui::Element DashboardView::render(const std::string& current_month,
                                     const tf::views::FocusController* focus) {
    using namespace ftxui;
    using tf::views::WidgetId;

    // Lambda: query the FocusController for a given WidgetId.  Returns
    // false when no controller was provided (e.g. preserves the v0.2
    // unfocused render byte-for-byte for tests / non-focusing callers).
    auto is_focused = [focus](WidgetId w) -> bool {
        return focus != nullptr && focus->is_widget_focused(w);
    };

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
        NetWorthBreakdownRenderer(checking, savings, credit, investment, net_worth,
                                  is_focused(WidgetId::NetWorth));

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
        CategorySpendingTrendsRenderer(category_trends, /*max_items*/5,
                                       is_focused(WidgetId::CategoryTrends));

    // ======================================================================
    // PANEL 3: ui_sync_status
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

    Element sync_status_panel =
        SyncStatusIndicatorRenderer(sync_statuses, is_focused(WidgetId::SyncStatus));

    // ======================================================================
    // COMPOSE: 2x2 responsive grid (post-shovel-removal layout).
    //   Top row:    [ NetWorth ][ SyncStatus ]
    //   Bottom row: [ CategoryTrends spanning ]
    // Each panel uses `| flex` so the row divides available width evenly;
    // `vbox(...) | flex` lets the dashboard fill the terminal's vertical
    // extent. Replacement widgets for the empty cell pending §3.3 of
    // greylock-kickoff.md.
    // ======================================================================
    return vbox({
        hbox({
            net_worth_panel | flex,
            sync_status_panel | flex,
        }),
        hbox({
            category_trends_panel | flex,
        }),
    }) | flex;
}
