#pragma once

// ---------------------------------------------------------------------------
// ui_shovel_score — Dashboard "Shovel Score" panel renderer.
// ---------------------------------------------------------------------------
// Renders the user's composite "Shovel Score" — a 0-100 number summarizing
// how aggressively the user is spending on AI-infrastructure suppliers
// ("shovels") — alongside the count of distinct shovel suppliers and the
// total dollar spend on those suppliers.
//
// PARAMETERS
//   score                 0-100 composite, ALREADY CLAMPED by the caller.
//                         The widget renders this verbatim; it does NOT
//                         re-clamp.  See DashboardView.cpp's
//                         compute_shovel_score() for the v0.2 stop-gap
//                         formula that produces this value, including
//                         why it is a placeholder and what a real model
//                         should look like.
//   supplier_count        Number of distinct shovel tickers discovered.
//                         Rendered as an integer.
//   total_shovel_spend    Total dollar spend across all shovel tickers,
//                         in dollars (NOT cents), as a non-negative double.
//
// NAMESPACE NOTE
//   These free functions live in `namespace ftxui` (rather than
//   `tf::widgets`) because they take only primitive types — there is no
//   widget-owned struct to put in tf::widgets.  Same rationale as
//   ui_net_worth.h.
//
// CALLERS
//   src/views/DashboardView.cpp (the only caller today).
// ---------------------------------------------------------------------------

#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

#include "../ViewCommon.h"  // KeyHint

namespace ftxui {

// Component wrapper for container slotting.  Currently unused.
Component ShovelScore(double score, int supplier_count, double total_shovel_spend);

// Build a single-frame FTXUI Element.  Pure function.
//
// `focused` (Task v0.3-1): when true, render with a yellow rounded
// border and the title in bright bold.  Default false preserves the v0.2
// visual byte-for-byte (existing snapshot fixtures unchanged).
Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend, bool focused = false);

// Task v0.3-5: contextual KeyHints for the StatusBar (v0.3-4).  Free
// function; see ui_net_worth.h for the namespace + ownership rationale.
std::vector<KeyHint> shovel_score_hints_when_focused();

} // namespace ftxui
