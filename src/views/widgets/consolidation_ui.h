#pragma once

// ---------------------------------------------------------------------------
// consolidation_ui — Dashboard "Bank Connections" + "Consolidated Net
// Worth" panel renderers.
// ---------------------------------------------------------------------------
// Two related renderers, kept in one file because both surface the
// ConsolidationService output: per-account connection status and an
// aggregate-of-aggregates net-worth panel.
//
// CURRENT STATUS
//   NOT wired into DashboardView in v0.2.  The Dashboard composes
//   ui_net_worth + ui_sync_status independently rather than the
//   consolidated equivalents below.  These widgets remain in the
//   tf_widgets target so the snapshot tests cover them and so a
//   v0.3 "consolidated view" panel can pick them up.  The
//   ConsolidationService is being refactored in parallel by the
//   discovery-engineer.
//
// PARAMETERS
//   accounts (BankConnectionStatus / *Renderer):
//     Vector of AccountConnection POD entries (see struct below).
//     Caller is responsible for ordering.
//   net_worth, checking, savings, credit, investment
//     (ConsolidatedNetWorth / *Renderer):
//     Dollar amounts (NOT cents) as plain doubles.  Sign convention
//     matches ui_net_worth: credit is negative when held as debt,
//     net_worth signed total.
//
// NAMESPACE NOTE
//   AccountConnection lives in `namespace tf::widgets` (Phase 5
//   isolation of widget-owned PODs).  The render functions stay in
//   `namespace ftxui`.  ConsolidatedNetWorth/*Renderer take only
//   primitives and therefore declare no struct.
//
// CALLERS
//   None in v0.2.  TODO(v0.3): wire into a "consolidated view" panel
//   or retire once ConsolidationService stabilizes.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

// One linked account's connection state.
//
// FIELDS
//   institution    Human-readable institution name ("Chase").
//   account_name   Per-account label ("Sapphire Checking", "Roth IRA").
//   balance        Account balance in dollars.  Signed per ui_net_worth
//                  convention (credit-card debt is negative).
//   connected      True if the institution is currently considered
//                  healthy.
//   last_sync      "YYYY-MM-DD" string.  Empty == never synced.
struct AccountConnection {
    std::string institution;
    std::string account_name;
    double balance;
    bool connected;
    std::string last_sync;
};

} // namespace tf::widgets

namespace ftxui {

// "Bank Connections" panel — per-account connection rows.
Component BankConnectionStatus(const std::vector<tf::widgets::AccountConnection>& accounts);
Element BankConnectionStatusRenderer(const std::vector<tf::widgets::AccountConnection>& accounts);

// "Consolidated Net Worth" panel — same shape as ui_net_worth but
// titled for the cross-institution-aggregate context.
Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment);
Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment);

} // namespace ftxui
