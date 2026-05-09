#pragma once

#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

struct CategoryTrend {
    std::string category_name;
    std::string emoji;
    double current_spend;
    double percent_change;
};

namespace ftxui {

Component CategorySpendingTrends(const std::vector<CategoryTrend>& trends, size_t max_items = 5);

Element CategorySpendingTrendsRenderer(const std::vector<CategoryTrend>& trends, size_t max_items);

} // namespace ftxui