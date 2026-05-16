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
#include "widgets/ui_cash_flow.h"
#include "widgets/ui_net_worth.h"
#include "widgets/ui_recent_activity.h"
#include "widgets/ui_sync_status.h"

namespace {

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
    // PANEL 2: ui_cash_flow
    // ----------------------------------------------------------------------
    // Sum positive-amount tx (income) and |negative-amount tx| (expenses)
    // for current_month across the visible-entity accounts.  Net is
    // income - expenses.  Filtered to the same entity scope as Net Worth.
    // ======================================================================
    double income_month   = 0.0;
    double expenses_month = 0.0;
    {
        std::set<std::string> visible_account_ids;
        if (!entity_id_.empty()) {
            for (auto* acc : data_store_.get_accounts_for_entity(entity_id_)) {
                visible_account_ids.insert(acc->id);
            }
        }
        for (const auto& tx : data_store_.transactions) {
            if (tx.date.substr(0, 7) != current_month) continue;
            if (!entity_id_.empty() &&
                visible_account_ids.find(tx.account_id) == visible_account_ids.end()) {
                continue;
            }
            if (tx.amount >= 0) income_month   += tx.amount;
            else                expenses_month += std::abs(tx.amount);
        }
    }
    const double net_month = income_month - expenses_month;
    Element cash_flow_panel =
        CashFlowThisMonthRenderer(income_month, expenses_month, net_month,
                                  is_focused(WidgetId::CashFlow));

    // ======================================================================
    // PANEL 3: ui_recent_activity
    // ----------------------------------------------------------------------
    // Last 5 transactions across the visible entity, most-recent first.
    // ======================================================================
    std::vector<tf::widgets::RecentTx> recent_rows;
    {
        std::set<std::string> visible_account_ids;
        if (!entity_id_.empty()) {
            for (auto* acc : data_store_.get_accounts_for_entity(entity_id_)) {
                visible_account_ids.insert(acc->id);
            }
        }
        // Copy + sort desc by date (string compare on YYYY-MM-DD is OK).
        std::vector<Transaction> ordered;
        ordered.reserve(data_store_.transactions.size());
        for (const auto& tx : data_store_.transactions) {
            if (!entity_id_.empty() &&
                visible_account_ids.find(tx.account_id) == visible_account_ids.end()) {
                continue;
            }
            ordered.push_back(tx);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const Transaction& a, const Transaction& b) {
                      return a.date > b.date;
                  });
        const size_t n = std::min<size_t>(ordered.size(), 5);
        for (size_t i = 0; i < n; ++i) {
            tf::widgets::RecentTx r;
            r.date        = ordered[i].date;
            r.description = ordered[i].description;
            r.amount      = ordered[i].amount;
            recent_rows.push_back(std::move(r));
        }
    }
    Element recent_activity_panel =
        RecentActivityRenderer(recent_rows, is_focused(WidgetId::RecentActivity));

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
    // COMPOSE: 2x2 responsive grid — Rory's four canonical widgets
    //   Top row:    [ NetWorth     ][ CashFlow   ]
    //   Bottom row: [ RecentActivity ][ SyncStatus ]
    // Each panel uses `| flex` so the row divides width evenly;
    // `vbox(...) | flex` lets the dashboard fill vertical extent.
    // See greylock-spec.md §8.3 / decisions Q3.
    // ======================================================================
    return vbox({
        hbox({
            net_worth_panel | flex,
            cash_flow_panel | flex,
        }),
        hbox({
            recent_activity_panel | flex,
            sync_status_panel | flex,
        }),
    }) | flex;
}
