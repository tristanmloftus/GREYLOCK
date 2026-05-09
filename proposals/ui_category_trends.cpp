#include "ui_category_trends.h"
#include <sstream>
#include <iomanip>

namespace ftxui {

Component CategorySpendingTrends(const std::vector<CategoryTrend>& trends, size_t max_items) {
    return Renderer([=] {
        return CategorySpendingTrendsRenderer(trends, max_items);
    });
}

Element CategorySpendingTrendsRenderer(const std::vector<CategoryTrend>& trends, size_t max_items) {
    std::vector<Element> rows;

    rows.push_back(text("Top Spending Categories") | bold);
    rows.push_back(separator());

    if (trends.empty()) {
        rows.push_back(text("  No transactions this month.") | dim);
    } else {
        size_t count = 0;
        for (const auto& t : trends) {
            if (count >= max_items) break;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);

            std::string direction;
            Color change_color = Color::White;
            if (t.percent_change > 0) {
                direction = "▲";
                change_color = Color::Red;
            } else if (t.percent_change < 0) {
                direction = "▼";
                change_color = Color::Green;
            } else {
                direction = "─";
            }

            oss.str("");
            oss << t.emoji << "  " << t.category_name << "  $" << t.current_spend << "  " << direction << " " << std::abs(t.percent_change) << "%";

            Element row = hbox({
                text(t.emoji) | dim,
                text("  " + t.category_name) | dim,
                text("  $" + std::to_string(t.current_spend)) | bold,
                text("  " + direction + " " + std::to_string(std::abs(t.percent_change)) + "%") | color(change_color)
            });
            rows.push_back(row);
            count++;
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui