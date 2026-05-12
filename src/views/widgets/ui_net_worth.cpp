// ---------------------------------------------------------------------------
// ui_net_worth.cpp — implementation of the Dashboard "Net Worth" panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  First line is a bold "Net Worth" header,
//   followed by a separator, then the headline total (bold + colored), a
//   blank spacer, and four right-aligned-label / left-aligned-value rows
//   for Checking, Savings, Credit, Investment.
//
// FORMATTING RULES
//   - All currency values use 2-decimal fixed precision and a leading
//     "$".  Negative values render as "-$N.NN" (sign before the symbol),
//     matching the v0.1 dashboard's accountant style.
//   - No thousands separators in v0.2 — preserved verbatim from v0.1.
//
// COLOR DISCIPLINE (semantics this widget assigns) — v0.3-5 migrated to kTokens
//   - kTokens.accent_positive  = positive net worth (you own more than you owe).
//   - kTokens.accent_negative  = negative net worth, OR a negative credit-card
//                                balance (debt currently held on the card).
//   - Default (kTokens.fg_default) = breakdown rows.
//   - Dim (FTXUI |dim modifier)    = field labels ("Checking:", "Savings:", ...).
//   Preserve the intent: net-worth coloring is a quick health glance;
//   credit-row red is a debt-present indicator only — it should NOT alarm
//   if the card is paid in full.
//
// EDGE CASES
//   - All zeros: renders cleanly as "$0.00" everywhere (no special path).
//   - Negative net worth: rendered in red bold.
//   - Positive credit balance (overpayment / credit-on-file): rendered
//     in the default color, NOT red (only credit < 0 turns red).
//
// CALLERS
//   src/views/DashboardView.cpp::render() — once per frame.
// ---------------------------------------------------------------------------

#include "ui_net_worth.h"

#include <iomanip>
#include <sstream>

#include "../ViewCommon.h"

namespace ftxui {

namespace {

// ---------------------------------------------------------------------------
// format_currency
// ---------------------------------------------------------------------------
// "12.34"  -> "$12.34"
// "-12.34" -> "-$12.34"
// 2-decimal fixed precision; no thousands separators.  Used by every row
// in the panel so the format stays consistent.
// ---------------------------------------------------------------------------
std::string format_currency(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (val < 0) {
        oss << "-$" << (-val);
    } else {
        oss << "$" << val;
    }
    return oss.str();
}
}  // namespace

// ---------------------------------------------------------------------------
// NetWorthBreakdown
// ---------------------------------------------------------------------------
// Component wrapper around NetWorthBreakdownRenderer.  Captures the input
// values by copy so the resulting Component can outlive the call site.
// Currently unused by DashboardView (which calls the renderer directly);
// kept for future container-based layouts.
// ---------------------------------------------------------------------------
Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth) {
    return Renderer([=] {
        return NetWorthBreakdownRenderer(checking, savings, credit, investment, net_worth);
    });
}

// ---------------------------------------------------------------------------
// NetWorthBreakdownRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function; no I/O.  Called once per frame by DashboardView::render().
//
// `focused` (Task v0.3-1): when true, the panel renders with a yellow
// rounded border and the title row in bright bold to signal that this
// widget has Dashboard focus.  When false, renders exactly as v0.2
// (default border, plain bold title) so existing snapshot fixtures
// remain byte-identical.
// ---------------------------------------------------------------------------
Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth, bool focused) {
    std::vector<Element> rows;

    // Title: bright bold + focus color when focused, plain bold otherwise.
    Element title = text("Net Worth") | bold;
    if (focused) title = title | color(kTokens.fg_emphasized);
    rows.push_back(title);
    rows.push_back(separator());

    // Headline total: accent_positive if non-negative, accent_negative if negative.
    const Color net_color = net_worth >= 0 ? kTokens.accent_positive : kTokens.accent_negative;
    rows.push_back(text(format_currency(net_worth)) | bold | color(net_color));
    rows.push_back(text(""));

    rows.push_back(hbox({ text("Checking:   ") | dim, text(format_currency(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text(format_currency(savings)) }));
    // Credit balances are negative liabilities; color as accent_negative
    // only when balance < 0.  (A positive credit value means credit-on-file
    // / overpayment, which is not a debt and therefore not styled as a
    // warning.)
    Element credit_value = text(format_currency(credit));
    if (credit < 0) credit_value = credit_value | color(kTokens.accent_negative);
    rows.push_back(hbox({ text("Credit:     ") | dim, credit_value }));
    rows.push_back(hbox({ text("Investment: ") | dim, text(format_currency(investment)) }));

    Element panel = vbox(std::move(rows));
    if (focused) {
        panel = panel | borderStyled(ROUNDED) | color(kTokens.focus);
    } else {
        panel = panel | border;
    }
    return panel;
}

// ---------------------------------------------------------------------------
// net_worth_hints_when_focused — v0.3-5 contextual StatusBar hints.
// ---------------------------------------------------------------------------
// Returns the per-widget hint list the StatusBar (v0.3-4) consumes when
// this panel is the focused widget.  Mirrors docs/UI_REDESIGN_V0.3.md §3d
// Example B and §3f's per-widget action table (Enter drills, r refreshes).
// ---------------------------------------------------------------------------
std::vector<KeyHint> net_worth_hints_when_focused() {
    return {
        {"Enter", "Drill into Net Worth"},
        {"r",     "Recompute"},
        {"Tab",   "Next widget"},
        {"j/k",   "Move down/up"},
        {"Esc",   "Unfocus"},
    };
}

} // namespace ftxui
