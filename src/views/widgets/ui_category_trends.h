// ui_category_trends — promoted from proposals/ in Phase 5.
//
// The CategoryTrend POD lives in namespace tf::widgets to avoid collision
// with any other CategoryTrend struct in the codebase (DashboardView.h
// previously declared one at global scope).

#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

struct CategoryTrend {
    std::string category_name;
    std::string emoji;
    double current_spend;
    double percent_change;
};

} // namespace tf::widgets

namespace ftxui {

Component CategorySpendingTrends(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items = 5);

Element CategorySpendingTrendsRenderer(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items = 5);

} // namespace ftxui
