#pragma once

// ---------------------------------------------------------------------------
// ui_net_worth — Dashboard "Net Worth" panel renderer.
// ---------------------------------------------------------------------------
// Renders a bordered text card showing the user's total net worth plus a
// per-account-type breakdown (Checking, Savings, Credit, Investment).
//
// PARAMETERS
//   checking, savings, credit, investment, net_worth
//     All are dollar amounts (NOT cents) as plain doubles.  Sign convention:
//       - Checking / Savings / Investment: positive = held value.
//       - Credit: negative = liability owed (red), positive = credit-on-file.
//       - net_worth: signed total; rendered green if >= 0, red if negative.
//
// NAMESPACE NOTE
//   These free functions live in `namespace ftxui` (rather than
//   `tf::widgets`) because they take only primitive types — there is no
//   widget-owned struct to put in tf::widgets, and the v0.1 convention
//   placed render helpers in the FTXUI namespace next to FTXUI's own
//   Element / Component types.  Widgets that DO own a POD (CategoryTrend,
//   SupplierSpend, etc.) declare that POD in `tf::widgets` to avoid the
//   global-scope collisions DashboardView.h had pre-Phase-5.
//
// CALLERS
//   src/views/DashboardView.cpp (the only caller today).
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

// Wrap the renderer in an FTXUI Component so it can be slotted into a
// component tree (e.g. for interactive containers).  Currently unused by
// DashboardView (which calls the renderer directly) but kept for future
// composability.
Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth);

// Build a single-frame FTXUI Element.  Pure function: same inputs always
// produce the same Element graph.
//
// `focused` (Task v0.3-1): when true, render with a yellow rounded
// border and the title row in bright bold.  Default false preserves the
// v0.2 visual byte-for-byte (existing snapshot fixtures unchanged).
Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth, bool focused = false);

} // namespace ftxui
