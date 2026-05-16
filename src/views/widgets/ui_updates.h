#pragma once

// ---------------------------------------------------------------------------
// ui_updates — generic (ticker, description) list renderer.
// ---------------------------------------------------------------------------
// Renders a simple bordered list of (ticker, description) pairs.
// Currently NOT wired into any view; kept in tf_widgets so the snapshot
// tests still cover the renderer and a future dashboard slot can pick it
// up without re-promoting from proposals/.
//
// PARAMETERS
//   suppliers   A vector of (ticker, description) pairs.  Both fields
//               are rendered verbatim — no parsing or coloring.
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
