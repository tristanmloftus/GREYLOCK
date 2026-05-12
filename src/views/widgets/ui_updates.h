#pragma once

// ---------------------------------------------------------------------------
// ui_updates — Dashboard "Discovered Suppliers (MoM Analysis)" panel
// renderer.
// ---------------------------------------------------------------------------
// Renders a simple bordered list of (ticker, description) pairs sourced
// from the DiscoveryService supplier map.  Originally proposed as a
// "what just got detected" feed for the Dashboard; promoted from
// proposals/ in Phase 5.
//
// CURRENT STATUS
//   NOT wired into DashboardView in v0.2.  The discovered-supplier
//   information is instead surfaced via ui_shovel_intelligence (which
//   adds MoM percent change + spend totals).  ui_updates remains in the
//   tf_widgets target so the snapshot tests still cover its renderer and
//   so a future v0.3 panel can pick it up without re-promoting from
//   proposals/.
//
// PARAMETERS
//   suppliers   A vector of (ticker, description) pairs.  Both fields
//               are rendered verbatim — no parsing or coloring.
//
// NAMESPACE NOTE
//   These free functions live in `namespace ftxui` (rather than
//   `tf::widgets`) because they take only primitive types — there is
//   no widget-owned struct to put in tf::widgets.
//
// CALLERS
//   None in v0.2.  Tests under tests/snapshot exercise the renderer
//   directly.  TODO(v0.3): either wire into a new dashboard slot or
//   remove if ui_shovel_intelligence remains the canonical feed.
// ---------------------------------------------------------------------------

#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

// Component wrapper for container slotting.  Currently unused.
Component SupplierTickerDisplay(const std::vector<std::pair<std::string, std::string>>& suppliers);

// Build a single-frame FTXUI Element.  Pure function.
Element SupplierTickerDisplayRenderer(
    const std::vector<std::pair<std::string, std::string>>& suppliers
);

} // namespace ftxui
