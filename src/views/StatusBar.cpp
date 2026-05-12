// ---------------------------------------------------------------------------
// StatusBar.cpp — two-row contextual footer.
// ---------------------------------------------------------------------------
// Task v0.3-4 EXTRACTION ONLY.  The bottom row matches the pre-v0.3
// global hint string byte-for-byte so existing snapshot fixtures of
// the App do not regress.  The top row is empty (renders as a single
// space-padded line) and reserved for v0.3-5 contextual hints.
//
// The contract for v0.3-5: when FocusController::context_hints()
// returns a non-empty vector, those strings are joined with two-space
// separators and rendered on the TOP row dimmed.  Until that lands,
// the top row is a blank placeholder so the bar's vertical height
// stays stable -- callers can render the App's vbox at a fixed row
// count regardless of whether v0.3-5 has shipped.

#include "StatusBar.h"

#include "FocusController.h"
#include "ViewCommon.h"

#include <ftxui/dom/elements.hpp>

#include <string>

namespace tf::views {

// ---------------------------------------------------------------------------
// global_hint_line — the canonical bottom-row hint string.  Lifted
// verbatim from the inline version in src/main.cpp (pre-v0.3-4).
// Changing this string is a snapshot-fixture-affecting change; existing
// fixtures must not regress.
// ---------------------------------------------------------------------------
const char* StatusBar::global_hint_line() {
    return "  [1-2] Switch entity  [Tab] Switch view  [P] Link Plaid  "
           "[L] Link test  [C] Config  [Q] Quit";
}

StatusBar::StatusBar() = default;

// ---------------------------------------------------------------------------
// render — two rows pinned beneath the main content.
// ---------------------------------------------------------------------------
ftxui::Element StatusBar::render(const FocusController& focus,
                                 std::string_view /*current_view_name*/) const {
    using namespace ftxui;

    // Top row: context hints from the FocusController.  v0.3-1 ships
    // this as an empty vector; v0.3-5 populates it.  When empty we
    // render a blank line so the vertical layout is stable.
    Element top_row;
    {
        const auto hints = focus.context_hints();
        if (hints.empty()) {
            top_row = text("");
        } else {
            std::string joined;
            for (std::size_t i = 0; i < hints.size(); ++i) {
                if (i) joined += "  ";
                joined += hints[i];
            }
            top_row = text("  " + joined) | dim;
        }
    }

    // Bottom row: the global hint string, byte-for-byte the same as
    // the v0.2 inline version.
    Element bottom_row = text(global_hint_line()) | dim;

    return vbox({
        top_row,
        bottom_row,
    });
}

}  // namespace tf::views
