#pragma once

#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

struct SupplierSpend {
    std::string ticker;
    std::string company_name;
    double amount;
    double percent_change;
};

namespace ftxui {

Component ShovelIntelligence(const std::vector<SupplierSpend>& suppliers);

Element ShovelIntelligenceRenderer(const std::vector<SupplierSpend>& suppliers);

} // namespace ftxui