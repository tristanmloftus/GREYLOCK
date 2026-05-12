// ---------------------------------------------------------------------------
// ui_shovel_intelligence.cpp — implementation of the "Shovel Intelligence"
// panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  Header "Shovel Intelligence" (bold) +
//   separator, then one hbox row per supplier:
//     <TICKER>  $<amount>   <arrow> <pct>% MoM
//   When the suppliers vector is empty, the body shows a two-line dim
//   helpful state:
//     "  No AI infrastructure detected."
//     "  Connect bank accounts to discover investments."
//
// FORMATTING RULES
//   - amount: 2-decimal fixed precision, leading "$", no thousands
//     separators.  Rendered via std::abs() so even a future signed input
//     stays positive on display.
//   - percent_change: 1-decimal fixed precision, magnitude only; sign
//     communicated by the arrow character.
//
// COLOR DISCIPLINE (semantics for THIS widget — note INVERSION below)
//   - Red ("^")  = shovel spend INCREASED MoM.
//   - Green ("v") = shovel spend DECREASED MoM.
//   - White ("-") = unchanged.
//   - Bold (no color) on ticker; dim on amount / "% MoM" suffix.
//
//   This matches ui_category_trends' convention (spending more = red),
//   NOT stock-market convention (price up = green).  The widget is
//   reporting EXPENSE direction, not equity performance — a user who
//   suddenly spends 50% more on cloud compute is using more shovels,
//   which the dashboard surfaces as a red warning.  The v0.3 redesign
//   must preserve this expense-direction semantic; do not flip to
//   market-style coloring.
//
// EDGE CASES
//   - Empty input: two dim lines (see VISUAL).  Acts as both empty state
//     and onboarding prompt.
//   - percent_change exactly 0: "-" arrow in default color.
//   - Negative `amount` (shouldn't happen given the caller's expense-
//     only filter): std::abs() guards the display.
//
// CALLERS
//   src/views/DashboardView.cpp::render() — once per frame.
// ---------------------------------------------------------------------------

#include "ui_shovel_intelligence.h"

#include <cmath>
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

// Format a percentage magnitude as "N.N" (no "%" suffix; caller appends).
std::string format_pct(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << val;
    return oss.str();
}
}  // namespace

// ---------------------------------------------------------------------------
// ShovelIntelligence
// ---------------------------------------------------------------------------
// Component wrapper around the renderer.  Captures the suppliers vector
// by copy so the resulting Component can outlive the call site.
// ---------------------------------------------------------------------------
Component ShovelIntelligence(const std::vector<tf::widgets::SupplierSpend>& suppliers) {
    return Renderer([suppliers] {
        return ShovelIntelligenceRenderer(suppliers);
    });
}

// ---------------------------------------------------------------------------
// ShovelIntelligenceRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function.  Called once per frame by DashboardView::render().
// ---------------------------------------------------------------------------
Element ShovelIntelligenceRenderer(const std::vector<tf::widgets::SupplierSpend>& suppliers) {
    std::vector<Element> rows;

    rows.push_back(text("Shovel Intelligence") | bold);
    rows.push_back(separator());

    if (suppliers.empty()) {
        rows.push_back(text("  No AI infrastructure detected.") | dim);
        rows.push_back(text("  Connect bank accounts to discover investments.") | dim);
    } else {
        for (const auto& s : suppliers) {
            // Sign of percent_change drives the arrow + color; magnitude
            // is rendered with abs().  See file header COLOR DISCIPLINE
            // for why "+" growth is red here (expense direction, not
            // equity performance).
            std::string direction;
            Color change_color = Color::White;
            if (s.percent_change > 0) {
                direction = "^";
                change_color = Color::Red;
            } else if (s.percent_change < 0) {
                direction = "v";
                change_color = Color::Green;
            } else {
                direction = "-";
            }

            Element row = hbox({
                text(s.ticker) | bold,
                text("  $" + format_amount(std::abs(s.amount))) | dim,
                text("  " + direction) | color(change_color),
                text(" " + format_pct(std::abs(s.percent_change)) + "% MoM") | dim
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
