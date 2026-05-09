#include "ui_shovel_intelligence.h"
#include <sstream>
#include <iomanip>

namespace ftxui {

Component ShovelIntelligence(const std::vector<SupplierSpend>& suppliers) {
    return Renderer([suppliers] {
        return ShovelIntelligenceRenderer(suppliers);
    });
}

Element ShovelIntelligenceRenderer(const std::vector<SupplierSpend>& suppliers) {
    std::vector<Element> rows;

    rows.push_back(text("Shovel Intelligence") | bold);
    rows.push_back(separator());

    if (suppliers.empty()) {
        rows.push_back(text("  No AI infrastructure detected.") | dim);
        rows.push_back(text("  Connect bank accounts to discover investments.") | dim);
    } else {
        for (const auto& s : suppliers) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);

            std::string direction;
            Color color = Color::White;
            if (s.percent_change > 0) {
                direction = "▲";
                color = Color::Red;
            } else if (s.percent_change < 0) {
                direction = "▼";
                color = Color::Green;
            } else {
                direction = "─";
            }

            oss.str("");
            oss << "  " << s.ticker << "  " << std::setw(10) << std::right << "$" << std::abs(s.amount);
            oss << "  " << direction << " " << std::abs(s.percent_change) << "% MoM";

            Element row = hbox({
                text(s.ticker) | bold,
                text("  $" + std::to_string(std::abs(s.amount))) | dim,
                text("  " + direction) | color,
                text(" " + std::to_string(std::abs(s.percent_change)) + "% MoM") | dim
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui