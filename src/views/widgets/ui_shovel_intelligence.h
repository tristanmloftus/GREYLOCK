// ui_shovel_intelligence — promoted from proposals/ in Phase 5.
//
// Renders the "Shovel Intelligence" panel: discovered AI-infrastructure
// suppliers (tickers like NVDA, AMZN) with their per-month spend and
// MoM percent change.  The SupplierSpend POD lives in namespace
// tf::widgets to avoid collision with other SupplierSpend structs.

#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

struct SupplierSpend {
    std::string ticker;
    std::string company_name;
    double amount;
    double percent_change;
};

} // namespace tf::widgets

namespace ftxui {

Component ShovelIntelligence(const std::vector<tf::widgets::SupplierSpend>& suppliers);

Element ShovelIntelligenceRenderer(const std::vector<tf::widgets::SupplierSpend>& suppliers);

} // namespace ftxui
