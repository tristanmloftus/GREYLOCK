#include "ui_updates.h"

namespace ftxui {

Component SupplierTickerDisplay(const std::vector<std::pair<std::string, std::string>>& suppliers) {
    return Renderer([suppliers] {
        return SupplierTickerDisplayRenderer(suppliers);
    });
}

Element SupplierTickerDisplayRenderer(
    const std::vector<std::pair<std::string, std::string>>& suppliers
) {
    std::vector<Element> rows;

    rows.push_back(text("Discovered Suppliers (MoM Analysis)") | bold);
    rows.push_back(separator());

    if (suppliers.empty()) {
        rows.push_back(text("  No suppliers discovered yet.") | dim);
    } else {
        for (const auto& [ticker, description] : suppliers) {
            const std::string row = "  " + ticker + "  |  " + description;
            rows.push_back(text(row));
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
