// test_modal_snapshot.cpp — Task v0.3-4 snapshot tests for the
// CommandPalette and HelpOverlay modal overlays.
//
// Two fixtures are produced at 120x40, the same canvas every other
// snapshot uses in this project:
//
//   - command_palette_open.txt : palette open with the query "tx",
//                                selection on the top fuzzy match.
//   - help_overlay.txt         : help overlay open showing the v0.3
//                                static cheat sheet.
//
// These tests intentionally do NOT include the underlying Dashboard
// view "behind" the palette -- only the modal element itself is
// captured at the fixed canvas size.  Composition over the background
// view is exercised manually by running the binary; testing the modal
// in isolation here keeps the fixtures decoupled from DataStore /
// account fixtures evolving.

#include "../src/views/CommandPalette.h"
#include "../src/views/HelpOverlay.h"

#include "snapshot/snapshot_helper.h"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <gtest/gtest.h>

using ftxui::Event;
using tf::views::CommandPalette;
using tf::views::HelpOverlay;

// ---------------------------------------------------------------------------
// CommandPalette snapshot — palette open with query "tx".
// ---------------------------------------------------------------------------
TEST(ModalSnapshot, CommandPaletteOpen_Tx) {
    CommandPalette p;
    p.open();
    p.handle_key(Event::Character("t"));
    p.handle_key(Event::Character("x"));

    // Wrap in a screen-sized container so the fixture is 120x40.
    using namespace ftxui;
    Element framed = vbox({
        filler(),
        hbox({ filler(), p.render(), filler() }),
        filler(),
    });

    tf::snapshot::ExpectMatchesFixture(framed, 120, 40, "command_palette_open");
}

// ---------------------------------------------------------------------------
// HelpOverlay snapshot.
// ---------------------------------------------------------------------------
TEST(ModalSnapshot, HelpOverlay_Open) {
    HelpOverlay h;
    h.open();

    using namespace ftxui;
    Element framed = vbox({
        filler(),
        hbox({ filler(), h.render(), filler() }),
        filler(),
    });

    tf::snapshot::ExpectMatchesFixture(framed, 120, 40, "help_overlay");
}
