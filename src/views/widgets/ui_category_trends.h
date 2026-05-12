#pragma once

// ---------------------------------------------------------------------------
// ui_category_trends — Dashboard "Top Spending Categories" panel renderer.
// ---------------------------------------------------------------------------
// Renders a bordered list of the user's top spending categories for the
// current month, each with a per-row month-over-month percent-change
// indicator.
//
// PARAMETERS
//   trends     A vector of CategoryTrend POD entries (see struct below).
//              Caller is responsible for ordering — this widget does NOT
//              sort.  DashboardView passes a pre-aggregated list keyed by
//              category_id (see DashboardView.cpp for the source query).
//   max_items  Defaults to 5.  Cap on rows rendered.  Excess entries are
//              silently dropped.
//
// NAMESPACE NOTE
//   The CategoryTrend POD lives in `namespace tf::widgets`.  This is a
//   Phase 5 fix: DashboardView.h previously declared a CategoryTrend at
//   global scope which collided with the v0.1 dashboard layout helpers
//   during the proposals/ promotion.  Moving the POD into tf::widgets
//   isolates per-widget types from global namespace and the render
//   functions stay in `namespace ftxui` because they take FTXUI types.
//
// CALLERS
//   src/views/DashboardView.cpp (the only caller today).
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

// One row of the trends panel.
//
// FIELDS
//   category_name   Human-readable name ("Food & Dining").  Free-form;
//                   not interpreted by the widget.
//   emoji           Short ASCII tag rendered as the row's "icon"
//                   ("[food]" in v0.2 — real emoji are deferred until
//                   the FTXUI grapheme-width story improves).
//   current_spend   Absolute-value expense total for the current month
//                   in dollars (NOT cents).  Always non-negative; the
//                   widget renders "$<value>".
//   percent_change  Signed MoM percent (e.g. 12.5 for +12.5%, -3.1 for
//                   -3.1%).  Sign drives the up/down arrow and color
//                   (see the .cpp color-discipline note).  Magnitude is
//                   rendered with abs() so the arrow carries the sign.
struct CategoryTrend {
    std::string category_name;
    std::string emoji;
    double current_spend;
    double percent_change;
};

} // namespace tf::widgets

namespace ftxui {

// Component wrapper for container slotting.  Currently unused.
Component CategorySpendingTrends(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items = 5);

// Build a single-frame FTXUI Element.  Pure function.
Element CategorySpendingTrendsRenderer(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items = 5);

} // namespace ftxui
