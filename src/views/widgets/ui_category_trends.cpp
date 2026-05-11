#include "ui_category_trends.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {
std::string format_amount(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val;
    return oss.str();
}

std::string format_pct(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << val;
    return oss.str();
}
}  // namespace

Component CategorySpendingTrends(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items) {
    return Renderer([trends, max_items] {
        return CategorySpendingTrendsRenderer(trends, max_items);
    });
}

Element CategorySpendingTrendsRenderer(const std::vector<tf::widgets::CategoryTrend>& trends, size_t max_items) {
    std::vector<Element> rows;

    rows.push_back(text("Top Spending Categories") | bold);
    rows.push_back(separator());

    if (trends.empty()) {
        rows.push_back(text("  No transactions this month.") | dim);
    } else {
        size_t count = 0;
        for (const auto& t : trends) {
            if (count >= max_items) break;

            std::string direction;
            Color change_color = Color::White;
            if (t.percent_change > 0) {
                direction = "^";
                change_color = Color::Red;
            } else if (t.percent_change < 0) {
                direction = "v";
                change_color = Color::Green;
            } else {
                direction = "-";
            }

            Element row = hbox({
                text(t.emoji) | dim,
                text("  " + t.category_name) | dim,
                text("  $" + format_amount(t.current_spend)) | bold,
                text("  " + direction + " " + format_pct(std::abs(t.percent_change)) + "%") | color(change_color)
            });
            rows.push_back(row);
            count++;
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
