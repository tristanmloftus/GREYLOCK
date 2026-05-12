// ---------------------------------------------------------------------------
// consolidation_ui.cpp — implementation of the "Bank Connections" and
// "Consolidated Net Worth" panels.
// ---------------------------------------------------------------------------
// VISUAL
//   Two independent bordered panels:
//
//   BankConnectionStatusRenderer:
//     Header "Bank Connections" + separator, then one hbox row per
//     linked account:
//       <[Connected]/[Disconnected]>  <institution> - <account_name>
//       $<balance>  Last sync: <last_sync>
//     Empty state: two dim lines onboarding the user to "Press [P]".
//
//   ConsolidatedNetWorthRenderer:
//     Header "Consolidated Net Worth" + separator, headline total
//     (bold, green/red by sign), a blank spacer, then four rows for
//     Checking, Savings, Credit, Investment.  Visually similar to
//     ui_net_worth but titled for the cross-institution aggregate
//     context.
//
// FORMATTING RULES
//   - balance / breakdown rows: 2-decimal fixed precision, leading "$".
//   - Negative balances render with a leading "-" before the "$" — same
//     accountant style as ui_net_worth.
//   - Status text: bracket-wrapped words ("[Connected]" / "[Disconnected]")
//     intentionally ASCII for terminal compatibility.
//
// COLOR DISCIPLINE (semantics for THESE widgets)
//   BankConnectionStatusRenderer:
//     - Green "[Connected]"    = healthy.
//     - Red   "[Disconnected]" = unhealthy.
//     Same standard health-status semantics as ui_sync_status.
//
//   ConsolidatedNetWorthRenderer:
//     - Green headline = net worth >= 0.
//     - Red   headline = net worth < 0.
//     - Breakdown rows: dim label, default-color value.  Notably this
//       widget does NOT red-color a negative credit balance (unlike
//       ui_net_worth which does).  Preserved verbatim from the v0.1
//       proposal — TODO(v0.3) decide whether to unify or keep distinct.
//
// EDGE CASES
//   - Empty accounts vector (BankConnectionStatus): onboarding placeholder.
//   - last_sync empty: rendered as "Never".
//   - All-zero breakdown (ConsolidatedNetWorth): renders "$0.00" everywhere.
//
// CALLERS
//   None in v0.2.  Snapshot tests cover both renderers directly.
//   TODO(v0.3): wire into a consolidated-view panel or retire.
// ---------------------------------------------------------------------------

#include "consolidation_ui.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {

// Format a dollar amount as "N.NN" (no "$" prefix; caller prepends).
std::string format_amount(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val;
    return oss.str();
}
}  // namespace

// ---------------------------------------------------------------------------
// BankConnectionStatus
// ---------------------------------------------------------------------------
// Component wrapper around BankConnectionStatusRenderer.  Captures the
// accounts vector by copy so the resulting Component outlives the call.
// ---------------------------------------------------------------------------
Component BankConnectionStatus(const std::vector<tf::widgets::AccountConnection>& accounts) {
    return Renderer([accounts] {
        return BankConnectionStatusRenderer(accounts);
    });
}

// ---------------------------------------------------------------------------
// BankConnectionStatusRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph for the "Bank Connections" panel.
// Pure function.  Snapshot-tested directly; currently no production
// callers.
// ---------------------------------------------------------------------------
Element BankConnectionStatusRenderer(const std::vector<tf::widgets::AccountConnection>& accounts) {
    std::vector<Element> rows;

    rows.push_back(text("Bank Connections") | bold);
    rows.push_back(separator());

    if (accounts.empty()) {
        rows.push_back(text("  No accounts connected.") | dim);
        rows.push_back(text("  Press [P] to link a bank account.") | dim);
    } else {
        for (const auto& acc : accounts) {
            const std::string status = acc.connected ? "[Connected]" : "[Disconnected]";
            const std::string last_sync = acc.last_sync.empty() ? "Never" : acc.last_sync;

            Element row = hbox({
                text(status) | color(acc.connected ? Color::Green : Color::Red),
                text(" "),
                text(acc.institution) | bold,
                text(" - ") | dim,
                text(acc.account_name),
                text("  $") | dim,
                text(format_amount(acc.balance)) | bold,
                text("  Last sync: ") | dim,
                text(last_sync)
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

// ---------------------------------------------------------------------------
// ConsolidatedNetWorth
// ---------------------------------------------------------------------------
// Component wrapper around ConsolidatedNetWorthRenderer.  Captures the
// inputs by copy so the resulting Component outlives the call.
// ---------------------------------------------------------------------------
Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment) {
    return Renderer([=] {
        return ConsolidatedNetWorthRenderer(net_worth, checking, savings, credit, investment);
    });
}

// ---------------------------------------------------------------------------
// ConsolidatedNetWorthRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph for the "Consolidated Net Worth" panel.
// Pure function.  Snapshot-tested directly; currently no production
// callers.  Visually parallel to ui_net_worth but titled for the
// cross-institution-aggregate context.
// ---------------------------------------------------------------------------
Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment) {
    std::vector<Element> rows;

    rows.push_back(text("Consolidated Net Worth") | bold);
    rows.push_back(separator());

    // Headline: green when net worth is non-negative, red otherwise.
    rows.push_back(text("$" + format_amount(net_worth)) | bold | color(net_worth >= 0 ? Color::Green : Color::Red));
    rows.push_back(text(""));

    rows.push_back(hbox({ text("Checking:   ") | dim, text("$" + format_amount(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text("$" + format_amount(savings)) }));
    // Note: unlike ui_net_worth, this widget does NOT red-color a
    // negative credit balance — preserved verbatim from the v0.1
    // proposal.  TODO(v0.3) reconcile with ui_net_worth's behavior.
    rows.push_back(hbox({ text("Credit:     ") | dim, text("$" + format_amount(credit)) }));
    rows.push_back(hbox({ text("Investment: ") | dim, text("$" + format_amount(investment)) }));

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
