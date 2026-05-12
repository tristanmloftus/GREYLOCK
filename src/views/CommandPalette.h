#pragma once

// ---------------------------------------------------------------------------
// CommandPalette — modal overlay (Task v0.3-4).
// ---------------------------------------------------------------------------
// A bottom-anchored, centered modal that captures the keyboard while
// open: typing edits the query, ↑/↓ (and j/k inside the palette) move
// the selection, Enter dispatches the highlighted command via a
// caller-supplied callback, Esc closes without dispatch, Backspace
// deletes the last query char.
//
// The class is intentionally light:
//   - Open/close lifecycle is the App's job; the palette only knows
//     "open" or "closed".  When the App calls open() it should also
//     call FocusController::enter_modal() to suppress legacy handlers.
//   - Fuzzy ranking is delegated to tf::utils::fuzzy_find().  results()
//     is a vector of indices into CommandRegistry::all_commands(), in
//     ranked order.
//   - Selection is an integer index into results(); arrow keys wrap.
//   - Rendering is a single ftxui::Element returned by render(); the
//     App composes it over the underlying view via FTXUI's `dbox`.
//
// The palette deliberately knows NOTHING about which actions to perform
// for each CommandId.  Dispatch is a CommandId -> void callback set by
// the App at construction (via set_dispatcher()); main.cpp's dispatch()
// switch maps the id back onto concrete app mutations.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3c "Command palette"
//   src/utils/CommandRegistry.{h,cpp}
//   src/views/FocusController.h (enter_modal / exit_modal)

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <string>
#include <vector>

#include "../utils/CommandRegistry.h"

namespace tf::views {

class CommandPalette {
public:
    using Dispatcher = std::function<void(tf::utils::CommandId)>;

    CommandPalette();

    // Set the callback the palette invokes when Enter is pressed on a
    // selected result.  Setting after construction (e.g. wired from
    // main.cpp's App ctor) is the canonical flow; tests use a lambda.
    void set_dispatcher(Dispatcher d);

    // Open the palette with an empty query.  Selection starts at 0.
    // Calling open() while already open is a no-op (preserves state).
    void open();

    // Close without dispatching.  Safe to call when already closed.
    void close();

    bool is_open() const noexcept;

    // Handle one FTXUI event.  Returns true iff the event was consumed.
    //
    // Recognised events:
    //   - Event::Escape          : close (no dispatch); consumed.
    //   - Event::Return          : dispatch selected + close; consumed
    //                              iff there is at least one result.
    //   - Event::ArrowDown / j   : selection + 1 (wrap); consumed.
    //   - Event::ArrowUp / k     : selection - 1 (wrap); consumed.
    //   - Event::Backspace       : pop the last query char; consumed.
    //   - Event::Character(c)    : append c to the query; consumed.
    //
    // Calling handle_key() while closed returns false.
    bool handle_key(const ftxui::Event& event);

    // Current query string (read-only; tests use this to assert state).
    const std::string& query() const noexcept;

    // Indices into all_commands() in ranked order.  Empty when no
    // results match; never larger than kMaxVisibleResults.
    const std::vector<int>& results() const noexcept;

    // Index into results() of the currently-selected entry.  Returns 0
    // when results is empty (callers should also check results()).
    int selected_index() const noexcept;

    // Render the palette as a centered, bordered FTXUI element.  Caller
    // wraps with FTXUI's `dbox` to composite over the background view.
    // Returns text("") when is_open() is false so callers can render
    // unconditionally without an explicit guard.
    ftxui::Element render() const;

    // Cap on the number of fuzzy-find results displayed.  Tests rely on
    // this value to compute wrap behaviour; bump cautiously.
    static constexpr int kMaxVisibleResults = 8;

private:
    bool                open_;
    std::string         query_;
    std::vector<int>    results_;
    int                 selected_;
    Dispatcher          dispatcher_;

    // Recompute results_ from the current query_.  Resets selected_ to
    // 0 (which clamps within the new results range).  Called on every
    // mutation of query_.
    void recompute_results();
};

}  // namespace tf::views
