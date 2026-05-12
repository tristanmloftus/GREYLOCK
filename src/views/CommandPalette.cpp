// ---------------------------------------------------------------------------
// CommandPalette.cpp — modal overlay implementation.
// ---------------------------------------------------------------------------
// See CommandPalette.h for the public contract.  Render layout follows
// docs/UI_REDESIGN_V0.3.md §3c (the mockup with the rounded border, ▸
// arrow on the selected row, right-aligned shortcut hint, and the
// footer "[Enter] Run  [↑↓] Select  [Esc] Cancel").

#include "CommandPalette.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <utility>

#include "ViewCommon.h"

namespace tf::views {

namespace {

// Width of the palette overlay (chars) and number of rows reserved
// below the query line for results + footer.  The numbers are tuned to
// the 120-column / 40-row fixture canvas used by the snapshot tests.
constexpr int kPaletteWidth   = 67;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------
CommandPalette::CommandPalette()
    : open_(false),
      selected_(0) {
}

void CommandPalette::set_dispatcher(Dispatcher d) {
    dispatcher_ = std::move(d);
}

void CommandPalette::open() {
    if (open_) return;
    open_     = true;
    query_.clear();
    selected_ = 0;
    recompute_results();
}

void CommandPalette::close() {
    open_     = false;
    query_.clear();
    selected_ = 0;
    results_.clear();
}

bool CommandPalette::is_open() const noexcept {
    return open_;
}

// ---------------------------------------------------------------------------
// recompute_results — pull from the registry's fuzzy_find each time the
// query changes.  Selection collapses to 0 (the new top result).
// ---------------------------------------------------------------------------
void CommandPalette::recompute_results() {
    results_  = tf::utils::fuzzy_find(query_, kMaxVisibleResults);
    selected_ = 0;
}

// ---------------------------------------------------------------------------
// handle_key — main event router.  See header for the recognised set.
// ---------------------------------------------------------------------------
bool CommandPalette::handle_key(const ftxui::Event& event) {
    using ftxui::Event;
    if (!open_) return false;

    // Esc: close without dispatching.  Consumed so the legacy Esc-exits
    // path never runs while a palette is up.
    if (event == Event::Escape) {
        close();
        return true;
    }

    // Enter: dispatch the selected command.  No-op (still consumed) if
    // there are no results so the user can't accidentally fall through
    // to a global handler with an empty palette open.
    if (event == Event::Return) {
        if (!results_.empty() && dispatcher_) {
            const int idx = results_[static_cast<std::size_t>(selected_)];
            const auto& cmd = tf::utils::all_commands()[
                static_cast<std::size_t>(idx)];
            const auto id = cmd.id;
            close();              // Close BEFORE invoking so the
                                  // dispatcher can re-open the palette
                                  // without state confusion.
            dispatcher_(id);
        } else {
            close();
        }
        return true;
    }

    // Arrow keys: wrap-around selection.  We deliberately do NOT bind
    // j/k here because the palette's query box should accept those
    // letters as input (a user typing "kf" to find "Refresh" must not
    // have the 'k' eaten by the selection state machine).  Vim-style
    // selection inside a typing context is a footgun.
    if (event == Event::ArrowDown) {
        if (!results_.empty()) {
            const int n = static_cast<int>(results_.size());
            selected_ = (selected_ + 1) % n;
        }
        return true;
    }
    if (event == Event::ArrowUp) {
        if (!results_.empty()) {
            const int n = static_cast<int>(results_.size());
            selected_ = (selected_ - 1 + n) % n;
        }
        return true;
    }

    // Backspace: pop last char.
    if (event == Event::Backspace) {
        if (!query_.empty()) {
            query_.pop_back();
            recompute_results();
        }
        return true;
    }

    // Character input: append to query.  FTXUI delivers most printable
    // ASCII chars via Event::Character(string); we accept the first
    // codepoint as a char (the palette is ASCII-only for v0.3-4).
    if (event.is_character()) {
        const auto& s = event.character();
        if (!s.empty()) {
            // Skip control chars and DEL just in case.
            const unsigned char c = static_cast<unsigned char>(s[0]);
            if (c >= 0x20 && c != 0x7f) {
                query_.push_back(static_cast<char>(c));
                recompute_results();
            }
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
const std::string& CommandPalette::query() const noexcept {
    return query_;
}

const std::vector<int>& CommandPalette::results() const noexcept {
    return results_;
}

int CommandPalette::selected_index() const noexcept {
    return selected_;
}

// ---------------------------------------------------------------------------
// render — centered bordered overlay.  Layout:
//
//   ┌─────────────────────────────────── PALETTE ─┐
//   │ : <query>                                   │
//   ├─────────────────────────────────────────────┤
//   │ ▸ Switch view: Dashboard          [Tab]     │  (selected; bold)
//   │   Switch view: Accounts           [Tab]     │
//   │   ...                                       │
//   └─────────────────────────────────────────────┘
//   [Enter] Run   [↑↓] Select   [Esc] Cancel
//
// The element is sized to kPaletteWidth and centered inside the
// caller's outer dbox.
// ---------------------------------------------------------------------------
ftxui::Element CommandPalette::render() const {
    using namespace ftxui;
    if (!open_) return text("");

    // Query line: a leading ":" + the typed query + a blinking-style
    // caret stand-in (just an underscore; FTXUI doesn't blink in
    // snapshot tests).
    Element query_line = hbox({
        text(": ") | color(kTokens.fg_dim),
        text(query_) | color(kTokens.fg_emphasized),
        text("_") | color(kTokens.fg_dim),
    });

    // Result rows.
    Elements rows;
    const auto& cmds = tf::utils::all_commands();
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const int reg_idx = results_[i];
        const auto& cmd = cmds[static_cast<std::size_t>(reg_idx)];

        const bool is_sel = (static_cast<int>(i) == selected_);
        const std::string arrow = is_sel ? "  " : "   ";   // 2 vs 3 cols
        const Color row_fg = is_sel ? kTokens.fg_emphasized : kTokens.fg_dim;

        // Two-column layout: [▸ name ...] [shortcut right-aligned].
        Element name_text = text(cmd.name) | color(row_fg);
        if (is_sel) name_text = name_text | bold;
        Element name_cell = hbox({
            text(is_sel ? "\xe2\x96\xb8 " : "  ") | color(kTokens.focus),
            name_text,
        });

        Element shortcut_cell =
            (cmd.shortcut && cmd.shortcut[0] != '\0')
                ? (text(std::string("[") + cmd.shortcut + "]") |
                       color(kTokens.fg_dim))
                : text("");

        rows.push_back(hbox({
            name_cell | flex,
            shortcut_cell,
        }));
    }

    // Empty-result hint -- so the user sees that their query matched
    // nothing rather than wondering why the box went blank.
    if (rows.empty()) {
        rows.push_back(text("  (no matches)") | color(kTokens.fg_dim));
    }

    Element body = vbox({
        query_line,
        separator() | color(kTokens.fg_dim),
        vbox(std::move(rows)),
    });

    // Border + title.
    Element framed = body | borderRounded | color(kTokens.fg_emphasized);

    // Footer hints (rendered as a second line below the bordered box).
    Element footer = hbox({
        text("[Enter] Run   ") | color(kTokens.fg_dim),
        text("[" "\xe2\x86\x91" "\xe2\x86\x93" "] Select   ")
            | color(kTokens.fg_dim),
        text("[Esc] Cancel") | color(kTokens.fg_dim),
    });

    Element overlay = vbox({
        framed,
        footer,
    });

    // Fix the palette width; the caller (the App) is responsible for
    // centering it within the screen with a dbox + filler() pattern.
    return overlay | size(WIDTH, EQUAL, kPaletteWidth);
}

}  // namespace tf::views
