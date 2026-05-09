#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <vector>
#include <string>

namespace ftxui {

Component SupplierTickerDisplay(const std::vector<std::pair<std::string, std::string>>& suppliers);

Element SupplierTickerDisplayRenderer(
    const std::vector<std::pair<std::string, std::string>>& suppliers
);

} // namespace ftxui