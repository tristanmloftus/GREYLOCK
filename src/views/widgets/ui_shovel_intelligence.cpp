#include "ui_shovel_intelligence.h"

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

Component ShovelIntelligence(const std::vector<tf::widgets::SupplierSpend>& suppliers) {
    return Renderer([suppliers] {
        return ShovelIntelligenceRenderer(suppliers);
    });
}

Element ShovelIntelligenceRenderer(const std::vector<tf::widgets::SupplierSpend>& suppliers) {
    std::vector<Element> rows;

    rows.push_back(text("Shovel Intelligence") | bold);
    rows.push_back(separator());

    if (suppliers.empty()) {
        rows.push_back(text("  No AI infrastructure detected.") | dim);
        rows.push_back(text("  Connect bank accounts to discover investments.") | dim);
    } else {
        for (const auto& s : suppliers) {
            std::string direction;
            Color change_color = Color::White;
            if (s.percent_change > 0) {
                direction = "^";
                change_color = Color::Red;
            } else if (s.percent_change < 0) {
                direction = "v";
                change_color = Color::Green;
            } else {
                direction = "-";
            }

            Element row = hbox({
                text(s.ticker) | bold,
                text("  $" + format_amount(std::abs(s.amount))) | dim,
                text("  " + direction) | color(change_color),
                text(" " + format_pct(std::abs(s.percent_change)) + "% MoM") | dim
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
