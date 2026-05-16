// ---------------------------------------------------------------------------
// HelpOverlay.cpp — static keybinding cheatsheet (Task v0.3-4).
// ---------------------------------------------------------------------------
// See HelpOverlay.h for the contract.  Content reproduces the §3f
// keybinding map mockup; v0.3-5 will tie the lower section to the
// focused widget's hints.

#include "HelpOverlay.h"

#include "ViewCommon.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <utility>
#include <vector>

namespace tf::views {

namespace {

// Single keybinding entry rendered as "key  description".  Width tuned
// to the 120x40 fixture canvas (the bordered box is 52 chars wide).
struct Binding {
    const char* key;
    const char* description;
};

// Static cheat sheet, sectioned by purpose. Top-level navigation is
// vim-style `g`+letter (greylock-spec.md Q14); per-view interaction
// stays on Tab / hjkl / Enter. Section dividers render as blank rows.
constexpr Binding kBindings[] = {
    { "",             "── Top-level navigation"        },
    { "g d",          "Dashboard"                      },
    { "g a",          "Accounts"                       },
    { "g t",          "Transactions"                   },
    { "g b",          "Budget"                         },
    { "g c",          "Categories"                     },
    { "",             ""                                },
    { "g n",          "Notes (v3 scaffold)"            },
    { "g D",          "Decisions (v3 scaffold)"        },
    { "g k",          "Tasks (v3 scaffold)"            },
    { "g e",          "Events (v3 scaffold)"           },
    { "g p",          "Proposals inbox (v4 scaffold)"  },
    { "g T",          "Targets (v4 scaffold)"          },
    { "g R",          "Relationships (v4 scaffold)"    },
    { "g r",          "Real Estate (v4 scaffold)"      },
    { "",             ""                                },
    { "1 / 2",        "Switch entity (Personal/PCC)"   },
    { ":",            "Command palette (fallback)"     },
    { "",             ""                                },
    { "",             "── Within a view"               },
    { "Tab / S-Tab",  "Cycle focus"                    },
    { "h j k l",      "Move focus L/D/U/R"             },
    { "Enter",        "Drill into focused widget"      },
    { "Esc",          "Back / close"                   },
    { "",             ""                                },
    { "",             "── Global"                      },
    { "?",            "This help"                      },
    { "q",            "Quit"                           },
};

constexpr int kOverlayWidth = 56;

}  // namespace

// ---------------------------------------------------------------------------
HelpOverlay::HelpOverlay()
    : open_(false) {
}

void HelpOverlay::open()       { open_ = true;  }
void HelpOverlay::close()      { open_ = false; }
bool HelpOverlay::is_open() const noexcept { return open_; }

// ---------------------------------------------------------------------------
// handle_key — Esc closes; every other key is consumed so the global
// handler chain doesn't fire under the open overlay.
// ---------------------------------------------------------------------------
bool HelpOverlay::handle_key(const ftxui::Event& event) {
    using ftxui::Event;
    if (!open_) return false;

    if (event == Event::Escape) {
        close();
        return true;
    }
    // Swallow everything else.  The user sees the cheat sheet; stray
    // typing must not slip past it into the App.
    return true;
}

// ---------------------------------------------------------------------------
// render — bordered cheat sheet, two-column body, centered footer hint.
// Caller (App::render) is responsible for centering the returned
// element on the screen via dbox + filler().
// ---------------------------------------------------------------------------
ftxui::Element HelpOverlay::render() const {
    using namespace ftxui;
    if (!open_) return text("");

    // Header strip ("─── KEYBINDINGS ───").  We render it as a plain
    // text line above the body; the border style around the whole box
    // is rounded.
    Element header = text(" KEYBINDINGS ") |
                     center |
                     color(kTokens.fg_emphasized) |
                     bold;

    Elements rows;
    rows.reserve(sizeof(kBindings) / sizeof(kBindings[0]) + 2);
    rows.push_back(text(""));   // blank line under header
    for (const auto& b : kBindings) {
        Element row = hbox({
            text(std::string("  ") + b.key) |
                color(kTokens.fg_emphasized) |
                size(WIDTH, EQUAL, 14),
            text(b.description) | color(kTokens.fg_default),
        });
        rows.push_back(row);
    }
    rows.push_back(text(""));
    rows.push_back(text("[Esc] Close") | center | color(kTokens.fg_dim));

    Element body = vbox({
        header,
        separator() | color(kTokens.fg_dim),
        vbox(std::move(rows)),
    });

    return body |
           borderRounded |
           color(kTokens.fg_emphasized) |
           size(WIDTH, EQUAL, kOverlayWidth);
}

}  // namespace tf::views
