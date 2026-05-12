#pragma once

// ---------------------------------------------------------------------------
// ui_shovel_intelligence — Dashboard "Shovel Intelligence" panel renderer.
// ---------------------------------------------------------------------------
// Renders a bordered list of discovered AI-infrastructure suppliers
// ("shovels": NVDA, AMZN, etc.) with per-supplier lifetime spend and
// month-over-month percent change.  The naming ("shovels") is the v0.2
// product framing: when there's a gold rush, sell shovels — track who
// the user is paying to build out their AI stack.
//
// PARAMETERS
//   suppliers   A vector of SupplierSpend POD entries.  Caller (currently
//               DashboardView::render) is responsible for sorting; this
//               widget renders in the given order.  v0.2 convention:
//               sort desc by `amount` so the largest-spend ticker is
//               at the top.
//
// NAMESPACE NOTE
//   SupplierSpend lives in `namespace tf::widgets` for the same Phase 5
//   reason as CategoryTrend (avoid global-scope POD collisions during
//   the proposals/ promotion).  Render functions stay in `namespace
//   ftxui` because they return FTXUI types.
//
// CALLERS
//   src/views/DashboardView.cpp (the only caller today).
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

// One discovered shovel supplier.
//
// FIELDS
//   ticker          Public-market ticker symbol ("NVDA", "AMZN").  Used
//                   verbatim as the row label.
//   company_name    Full company name (e.g. "NVIDIA Corp.").  Currently
//                   accepted but NOT rendered by the widget — kept on
//                   the struct so the DiscoveryService→widget pipeline
//                   can populate it without a future schema change.
//                   TODO(v0.3): render or remove.
//   amount          Lifetime absolute-value spend in dollars (NOT cents),
//                   summed across all detected transactions for this
//                   ticker.  Always non-negative; the widget calls
//                   std::abs() defensively before rendering.
//   percent_change  Signed month-over-month percent change for this
//                   ticker.  Sign drives the arrow + color (see the
//                   .cpp color-discipline note).
struct SupplierSpend {
    std::string ticker;
    std::string company_name;
    double amount;
    double percent_change;
};

} // namespace tf::widgets

namespace ftxui {

// Component wrapper for container slotting.  Currently unused.
Component ShovelIntelligence(const std::vector<tf::widgets::SupplierSpend>& suppliers);

// Build a single-frame FTXUI Element.  Pure function.
//
// `focused` (Task v0.3-1): when true, render with a yellow rounded
// border and the title in bright bold.  Default false preserves the v0.2
// visual byte-for-byte (existing snapshot fixtures unchanged).
Element ShovelIntelligenceRenderer(const std::vector<tf::widgets::SupplierSpend>& suppliers, bool focused = false);

} // namespace ftxui
