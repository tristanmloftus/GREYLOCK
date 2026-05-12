#pragma once

// ---------------------------------------------------------------------------
// StatusBar — two-row contextual footer (Task v0.3-4 extraction step).
// ---------------------------------------------------------------------------
// Replaces the inline status-bar block previously living in src/main.cpp
// (the `text("  [1-2] Switch entity ...") | dim` block).  See
// docs/UI_REDESIGN_V0.3.md §3d for the long-form design.
//
// This task (v0.3-4) does ONLY the extraction: the top row is reserved
// for v0.3-5 contextual hints and renders empty for now (or a "no
// hints" placeholder).  The bottom row is the existing global key hint
// set, unchanged.
//
// SHAPE for v0.3-5
//   When the contextual-hints layer lands, the App will populate
//   set_focus_hints(WidgetId, std::vector<KeyHint>) before render() and
//   the top row will draw the active widget's hints.  The signature of
//   render() is forward-compatible: it accepts the FocusController so
//   v0.3-5 can pull context_hints() without churning the call site.
//
// CALLER CONTRACT
//   - render(focus, current_view_name) returns an FTXUI Element pinned
//     to two rows of height.  The App composes it as the last
//     non-status element in its vbox.
//   - render() reads focus.context_hints() if non-empty; v0.3-1 left
//     this as a stub returning empty, so the top row is BLANK until
//     v0.3-5 fills it in.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3d "Status bar redesign"
//   src/views/FocusController.h (context_hints stub)

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>

namespace tf::views {

class FocusController;   // forward decl; full include in StatusBar.cpp.

class StatusBar {
public:
    StatusBar();

    // Render the two-row status bar.  `current_view_name` is the human-
    // readable label of the active view ("Dashboard", "Accounts", ...)
    // -- v0.3-5 may decorate the top row with it; v0.3-4 ignores it
    // but the param is kept so future tasks don't churn the call site.
    ftxui::Element render(const FocusController& focus,
                          std::string_view current_view_name) const;

private:
    // Global key hint string -- the bottom row, unchanged from v0.2.
    // Pulled out as a constant so it lives in one place and tests can
    // depend on it.
    static const char* global_hint_line();
};

}  // namespace tf::views
