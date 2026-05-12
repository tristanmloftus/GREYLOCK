// ---------------------------------------------------------------------------
// ui_updates.cpp — implementation of the "Discovered Suppliers (MoM Analysis)"
// panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  Header "Discovered Suppliers (MoM Analysis)"
//   (bold) + separator, then one default-colored row per supplier:
//     "  <TICKER>  |  <description>"
//   Empty state is a single dim line:
//     "  No suppliers discovered yet."
//
// FORMATTING RULES
//   - Both ticker and description are rendered verbatim — no truncation,
//     no case normalization, no padding.
//   - Pipe separator " | " is fixed; rows do NOT align across the list.
//   - No currency, no percentages — this widget shows discovery output,
//     not financial figures.
//
// COLOR DISCIPLINE
//   - Header: bold (default color).
//   - Body rows: default color (no semantic coloring).
//   - Empty-state line: dim.
//   Because this widget is informational-only (no actionable
//   good/bad state) it does not use the dashboard's red/green palette.
//   The v0.3 redesign should keep this widget neutral; if a future
//   variant needs to signal "new this sync" vs. "previously seen",
//   introduce a dedicated badge rather than recoloring the rows.
//
// EDGE CASES
//   - Empty input: dim "No suppliers discovered yet." placeholder.
//   - Very long ticker / description: truncated by FTXUI at the panel
//     border, NOT by this widget.  Acceptable for v0.2 where ticker
//     length is at most ~5 chars.
//
// CALLERS
//   None in v0.2.  Snapshot tests cover the renderer directly.
//   TODO(v0.3): wire into a dashboard slot or retire.
// ---------------------------------------------------------------------------

#include "ui_updates.h"

namespace ftxui {

// ---------------------------------------------------------------------------
// SupplierTickerDisplay
// ---------------------------------------------------------------------------
// Component wrapper around the renderer.  Captures the suppliers vector
// by copy so the resulting Component can outlive the call site.
// ---------------------------------------------------------------------------
Component SupplierTickerDisplay(const std::vector<std::pair<std::string, std::string>>& suppliers) {
    return Renderer([suppliers] {
        return SupplierTickerDisplayRenderer(suppliers);
    });
}

// ---------------------------------------------------------------------------
// SupplierTickerDisplayRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function.  Called by snapshot tests today; intended for a dashboard
// slot in v0.3.
// ---------------------------------------------------------------------------
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
