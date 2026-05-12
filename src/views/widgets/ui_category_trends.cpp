// ---------------------------------------------------------------------------
// ui_category_trends.cpp — implementation of the "Top Spending Categories"
// panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  Header "Top Spending Categories" (bold) +
//   separator, then up to max_items rows.  Each row is a single hbox:
//     <emoji>  <category_name>  $<current_spend>   <arrow> <pct>%
//   When the trends vector is empty, the body becomes a single dim line:
//     "  No transactions this month."
//
// FORMATTING RULES
//   - current_spend: 2-decimal fixed precision, leading "$", no thousands
//     separators (matches ui_net_worth).
//   - percent_change: 1-decimal fixed precision; sign is communicated by
//     the arrow character, NOT by a leading minus.  Magnitude is rendered
//     with std::abs() so "v 3.1%" reads "down 3.1%".
//
// COLOR DISCIPLINE (semantics for THIS widget) — v0.3-5 migrated to kTokens
//   - kTokens.thesis_up ("^")        = spending increased month-over-month.
//                                      Magenta: per the v0.3 redesign, a
//                                      category trending UP is not "bad"
//                                      — it is "interesting / thesis-
//                                      confirming" (where is the money
//                                      actually going).  The owner is
//                                      welcome to flip this back to
//                                      accent_warning in v0.4 if "spending
//                                      up = bad" becomes the operative
//                                      framing again.
//   - kTokens.accent_positive ("v") = spending decreased MoM (good news /
//                                      savings — green/teal).
//   - kTokens.fg_dim ("-")          = unchanged (percent_change == 0).
//
// EDGE CASES
//   - Empty input: shows the dim "No transactions this month." placeholder
//     rather than a blank bordered box.
//   - More than max_items entries: silently truncated; no "+N more" hint
//     (deferred to v0.3 once the categories taxonomy stabilizes).
//   - percent_change exactly 0: "-" arrow in default color.
//
// CALLERS
//   src/views/DashboardView.cpp::render() — once per frame.
// ---------------------------------------------------------------------------

#include "ui_category_trends.h"

#include <cmath>
#include <iomanip>
#include <sstream>

#include "../ViewCommon.h"

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
// CategorySpendingTrends
// ---------------------------------------------------------------------------
// Component wrapper around the renderer.  Captures inputs by copy so the
// resulting Component can outlive the call site.  Currently unused.
// ---------------------------------------------------------------------------
Component CategorySpendingTrends(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items) {
    return Renderer([trends, max_items] {
        return CategorySpendingTrendsRenderer(trends, max_items);
    });
}

// ---------------------------------------------------------------------------
// CategorySpendingTrendsRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function.  Called once per frame by DashboardView::render().
// ---------------------------------------------------------------------------
Element CategorySpendingTrendsRenderer(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items, bool focused) {
    std::vector<Element> rows;

    // Title: bright bold + focus color when focused, plain bold otherwise.
    Element title = text("Top Spending Categories") | bold;
    if (focused) title = title | color(kTokens.fg_emphasized);
    rows.push_back(title);
    rows.push_back(separator());

    if (trends.empty()) {
        rows.push_back(text("  No transactions this month.") | dim);
    } else {
        size_t count = 0;
        for (const auto& t : trends) {
            if (count >= max_items) break;

            // Arrow + color encode the SIGN of percent_change; the
            // numeric magnitude is always rendered as a positive number.
            // See the "COLOR DISCIPLINE" note in the file header for why
            // spend-up is `thesis_up` (magenta) and spend-down is
            // `accent_positive` (savings = good = green/teal) here.
            std::string direction;
            Color change_color = kTokens.fg_dim;
            if (t.percent_change > 0) {
                direction = "^";
                change_color = kTokens.thesis_up;
            } else if (t.percent_change < 0) {
                direction = "v";
                change_color = kTokens.accent_positive;
            } else {
                direction = "-";
            }

            Element row = hbox({
                text(t.emoji) | dim,
                text("  " + t.category_name) | dim,
                text("  $" + format_amount(t.current_spend)) | bold,
                text("  " + direction + " " + format_pct(std::abs(t.percent_change)) + "%") | color(change_color)
            });
            rows.push_back(row);
            count++;
        }
    }

    Element panel = vbox(std::move(rows));
    if (focused) {
        panel = panel | borderStyled(ROUNDED) | color(kTokens.focus);
    } else {
        panel = panel | border;
    }
    return panel;
}

} // namespace ftxui
