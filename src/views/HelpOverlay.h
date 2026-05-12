#pragma once

// ---------------------------------------------------------------------------
// HelpOverlay — modal keybinding cheatsheet (Task v0.3-4).
// ---------------------------------------------------------------------------
// Single-screen overlay that prints the v0.3 keybinding table.  Opens
// on `?` (also via `:help` in the command palette), closes on Esc.
//
// SCOPE for v0.3-4:
//   - Static content drawn from docs/UI_REDESIGN_V0.3.md §3f
//     (keybinding map).  v0.3-5 ties the bottom section to the focused
//     widget's hints, but the static cheat sheet is the canonical
//     surface for "what keys does this app respond to?".
//   - Esc closes.  No scrolling -- the v0.3 binding set fits in one
//     screen at 120x40.
//
// The class is intentionally tiny.  The App owns the instance; events
// route through handle_key() while is_open() is true.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3c "?" + ":help" (Q6 -- ship both)
//   docs/UI_REDESIGN_V0.3.md §3f keybinding map
//   src/views/FocusController.h (enter_modal / exit_modal)

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace tf::views {

class HelpOverlay {
public:
    HelpOverlay();

    void open();
    void close();
    bool is_open() const noexcept;

    // Handle one event.  Returns true iff consumed.
    // Recognised events: Esc (close).  All other events are CONSUMED
    // while open (returns true) so the user's stray Tab/q/etc. don't
    // leak through and trigger global handlers while the cheat sheet
    // is visible.
    bool handle_key(const ftxui::Event& event);

    // Render the cheat sheet.  Returns text("") when closed so callers
    // can render unconditionally.
    ftxui::Element render() const;

private:
    bool open_;
};

}  // namespace tf::views
