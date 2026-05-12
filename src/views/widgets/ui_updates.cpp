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
// COLOR DISCIPLINE — v0.3-5 migrated to kTokens
//   - Header: bold (default color).
//   - Ticker symbol:     kTokens.fg_emphasized (the supplier name — the
//                                                primary data on each row).
//   - Separator + body:  kTokens.fg_dim         (neutral metadata).
//   - Empty-state line:  dim.
//   Because this widget is informational-only (no actionable good/bad
//   state) it does not use the red/green/magenta semantic accents.  If a
//   future variant needs to signal "new this sync" vs. "previously seen",
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

#include "../ViewCommon.h"

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
            // Ticker = supplier name = fg_emphasized; description and
            // separator are neutral metadata = fg_dim.
            rows.push_back(hbox({
                text("  "),
                text(ticker) | color(kTokens.fg_emphasized),
                text("  |  ") | color(kTokens.fg_dim),
                text(description) | color(kTokens.fg_dim),
            }));
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
