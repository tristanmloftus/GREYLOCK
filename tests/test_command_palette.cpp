// test_command_palette.cpp — Task v0.3-4 unit tests for CommandRegistry and
// CommandPalette.  Two suites:
//
//   CommandRegistry — pure data tests on the 20-entry registry and the
//                     fuzzy_find ranker (no FTXUI dependency).
//
//   CommandPalette  — selection-state and dispatch tests on the modal
//                     overlay component.  Construction is cheap (no
//                     FTXUI rendering on the hot path of these tests),
//                     so we exercise open/close + selection wrap +
//                     Enter-dispatches without spinning up a Screen.
//
// See:
//   src/utils/CommandRegistry.h
//   src/views/CommandPalette.h
//   docs/UI_REDESIGN_V0.3.md §3c

#include "../src/utils/CommandRegistry.h"
#include "../src/views/CommandPalette.h"

#include <ftxui/component/event.hpp>

#include <gtest/gtest.h>

#include <string>

using tf::utils::CommandId;
using tf::utils::all_commands;
using tf::utils::fuzzy_find;
using tf::utils::kCommandCount;
using tf::views::CommandPalette;
using ftxui::Event;

// ===========================================================================
// CommandRegistry tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. The registry has exactly 20 entries, each with a stable CommandId
//    matching its declaration position, non-empty name, non-empty alias.
// ---------------------------------------------------------------------------
TEST(CommandRegistry, Registry_HasAllExpectedCommands) {
    const auto& cmds = all_commands();
    ASSERT_EQ(cmds.size(), kCommandCount);
    ASSERT_EQ(kCommandCount, 20u);

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        EXPECT_EQ(static_cast<int>(cmds[i].id), static_cast<int>(i))
            << "CommandId order drifted at index " << i;
        ASSERT_NE(cmds[i].name,  nullptr);
        ASSERT_NE(cmds[i].alias, nullptr);
        ASSERT_NE(cmds[i].shortcut, nullptr);
        EXPECT_GT(std::string(cmds[i].name).size(), 0u);
        EXPECT_GT(std::string(cmds[i].alias).size(), 0u);
    }

    // Smoke-check a few canonical entries.
    EXPECT_STREQ(cmds[static_cast<int>(CommandId::SwitchView_Dashboard)].alias,
                 "dashboard");
    EXPECT_STREQ(cmds[static_cast<int>(CommandId::SwitchView_Transactions)].alias,
                 "tx");
    EXPECT_STREQ(cmds[static_cast<int>(CommandId::Quit)].alias, "quit");
    EXPECT_STREQ(cmds[static_cast<int>(CommandId::Search_Transactions)].alias,
                 "search");
}

// ---------------------------------------------------------------------------
// 2. Query "dashboard" puts SwitchView_Dashboard at position 0.
//
//    This is the alias-exact-match path; the alias literally IS the
//    query, so the score must be the highest in the table.
// ---------------------------------------------------------------------------
TEST(CommandRegistry, Fuzzy_QueryDashboard_ReturnsSwitchToDashboardFirst) {
    const auto hits = fuzzy_find("dashboard", 8);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.front(),
              static_cast<int>(CommandId::SwitchView_Dashboard));
}

// ---------------------------------------------------------------------------
// 3. Query "tx" puts SwitchView_Transactions at position 0.
//
//    Aliases "transactions" and "plaid-test" both contain t and x in
//    order; the camelCase/separator bonuses and lower unmatched penalty
//    on "transactions" make it the winner.
// ---------------------------------------------------------------------------
TEST(CommandRegistry, Fuzzy_QueryTx_ReturnsTransactionsFirst) {
    const auto hits = fuzzy_find("tx", 8);
    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits.front(),
              static_cast<int>(CommandId::SwitchView_Transactions));
}

// ---------------------------------------------------------------------------
// 4. Empty query returns the first `max_results` indices in declaration
//    order.  This is the palette's idle state.
// ---------------------------------------------------------------------------
TEST(CommandRegistry, Fuzzy_EmptyQuery_ReturnsTopNDefaults) {
    const auto hits = fuzzy_find("", 5);
    ASSERT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0], 0);
    EXPECT_EQ(hits[1], 1);
    EXPECT_EQ(hits[2], 2);
    EXPECT_EQ(hits[3], 3);
    EXPECT_EQ(hits[4], 4);

    // max_results larger than the registry caps at kCommandCount.
    const auto all = fuzzy_find("", 999);
    EXPECT_EQ(all.size(), kCommandCount);
}

// ===========================================================================
// CommandPalette tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 5. Arrow keys (up/down) cycle through the result list with wrap.
//
//    Opens the palette with an empty query (default 8 results); presses
//    ArrowDown enough times so the selection lands on the last entry,
//    then one more ArrowDown should wrap to index 0.  Symmetric for
//    ArrowUp.
// ---------------------------------------------------------------------------
TEST(CommandPalette, Selection_ArrowKeysWrap) {
    CommandPalette p;
    p.open();
    ASSERT_TRUE(p.is_open());

    const auto& results = p.results();
    ASSERT_GE(results.size(), 2u)
        << "Need at least 2 default results to exercise wrap";

    // Initially the first result is selected.
    EXPECT_EQ(p.selected_index(), 0);

    // Walk down to the last result.
    const int n = static_cast<int>(results.size());
    for (int i = 1; i < n; ++i) {
        EXPECT_TRUE(p.handle_key(Event::ArrowDown));
        EXPECT_EQ(p.selected_index(), i);
    }
    // One more down wraps to 0.
    EXPECT_TRUE(p.handle_key(Event::ArrowDown));
    EXPECT_EQ(p.selected_index(), 0);

    // Up from 0 wraps to n-1.
    EXPECT_TRUE(p.handle_key(Event::ArrowUp));
    EXPECT_EQ(p.selected_index(), n - 1);
}

// ---------------------------------------------------------------------------
// 6. Pressing Enter dispatches the currently-selected command via the
//    callback supplied to set_dispatcher().  The palette CLOSES on
//    Enter.  We type "dashboard" so the first result is the Dashboard
//    switch; Enter must deliver CommandId::SwitchView_Dashboard.
// ---------------------------------------------------------------------------
TEST(CommandPalette, Selection_EnterDispatchesSelectedCommand) {
    CommandPalette p;
    CommandId dispatched = CommandId::Help;       // any sentinel != target
    bool      called     = false;
    p.set_dispatcher([&](CommandId id) {
        dispatched = id;
        called     = true;
    });

    p.open();
    // Type the query "dashboard" one char at a time -- the palette
    // re-runs fuzzy_find on each keystroke.
    for (char c : std::string("dashboard")) {
        EXPECT_TRUE(p.handle_key(Event::Character(std::string(1, c))));
    }
    ASSERT_FALSE(p.results().empty());
    EXPECT_EQ(p.results().front(),
              static_cast<int>(CommandId::SwitchView_Dashboard));

    EXPECT_TRUE(p.handle_key(Event::Return));
    EXPECT_TRUE(called) << "Dispatcher was not invoked";
    EXPECT_EQ(dispatched, CommandId::SwitchView_Dashboard);
    EXPECT_FALSE(p.is_open()) << "Palette must close after Enter";
}
