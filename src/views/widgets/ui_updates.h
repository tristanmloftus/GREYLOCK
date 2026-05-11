// ui_updates — promoted from proposals/ in Phase 5.
//
// Renders the discovered-suppliers ticker display (ticker + description
// pairs).  Currently not wired into DashboardView; kept available for
// future panel use.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

Component SupplierTickerDisplay(const std::vector<std::pair<std::string, std::string>>& suppliers);

Element SupplierTickerDisplayRenderer(
    const std::vector<std::pair<std::string, std::string>>& suppliers
);

} // namespace ftxui
