// test_focus_controller.cpp — Task v0.3-1 unit tests for the Dashboard
// focus state machine.  Pure state-machine tests; no FTXUI rendering is
// involved.  See src/views/FocusController.h for the public contract
// being exercised here.

#include "../src/views/FocusController.h"

#include <ftxui/component/event.hpp>

#include <gtest/gtest.h>

using tf::views::FocusController;
using tf::views::FocusLevel;
using tf::views::WidgetId;
using ftxui::Event;

namespace {

// Helper: assert that after `tabs` Tab presses from a freshly-constructed
// controller, the focused widget matches `expected`.
void TabNTimes(FocusController& fc, int n) {
    for (int i = 0; i < n; ++i) {
        EXPECT_TRUE(fc.handle_key(Event::Tab))
            << "Tab #" << (i + 1) << " was not consumed";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Initial state — Dashboard level, no widget focused.
// ---------------------------------------------------------------------------
TEST(FocusController, InitialState_DashboardLevelNoWidget) {
    FocusController fc;
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
    EXPECT_FALSE(fc.is_widget_focused(WidgetId::NetWorth));
}

// ---------------------------------------------------------------------------
// 2. Tab from Dashboard — lands on the first widget (NetWorth).
// ---------------------------------------------------------------------------
TEST(FocusController, TabFromDashboard_LandsOnFirstWidget) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.level(), FocusLevel::Widget);
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// 3. Tab cycles in declaration order through all five widgets, then wraps
//    to NetWorth.  Total: 5 Tabs to reach each widget, 6th Tab wraps.
// ---------------------------------------------------------------------------
TEST(FocusController, TabFromWidget_AdvancesInOrder) {
    FocusController fc;
    // Tab #1: Dashboard -> Widget(NetWorth)
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    // Tab #2..5: cycle through the remaining four widgets in order.
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelScore);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelIntelligence);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::CategoryTrends);
    // Tab #6: wraps to first widget (NetWorth).
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// 4. Shift-Tab from Dashboard lands on the LAST widget (CategoryTrends),
//    then cycles backward and wraps.
// ---------------------------------------------------------------------------
TEST(FocusController, ShiftTabReverseOrder) {
    FocusController fc;
    // Shift-Tab #1 from Dashboard -> Widget(CategoryTrends).
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.level(), FocusLevel::Widget);
    EXPECT_EQ(fc.focused_widget(), WidgetId::CategoryTrends);
    // Backward cycle through remaining widgets.
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelIntelligence);
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelScore);
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    // Wrap back to CategoryTrends.
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::CategoryTrends);
}

// ---------------------------------------------------------------------------
// 5. hjkl Right (l) and ArrowRight move across the focused widget's row.
//    From NetWorth (row 0, col 0) -> ShovelScore (row 0, col 1).
// ---------------------------------------------------------------------------
TEST(FocusController, HjklRight_MovesAcrossRow) {
    FocusController fc;
    TabNTimes(fc, 1);  // Land on NetWorth.
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);

    EXPECT_TRUE(fc.handle_key(Event::Character('l')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelScore);

    // ArrowRight equivalence.
    EXPECT_TRUE(fc.handle_key(Event::ArrowRight));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
}

// ---------------------------------------------------------------------------
// 6. h/ArrowLeft at the leftmost cell wraps to the rightmost in the same
//    row.  From NetWorth (row 0, col 0), Left -> SyncStatus (row 0, col 2).
// ---------------------------------------------------------------------------
TEST(FocusController, HjklLeftAtEdgeWraps) {
    FocusController fc;
    TabNTimes(fc, 1);  // NetWorth.
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);

    EXPECT_TRUE(fc.handle_key(Event::Character('h')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);

    // ArrowLeft from SyncStatus lands on ShovelScore (col 1) -- standard
    // intra-row move, no wrap.
    EXPECT_TRUE(fc.handle_key(Event::ArrowLeft));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelScore);
}

// ---------------------------------------------------------------------------
// 7. j/ArrowDown moves to the same column in the next row.  From
//    NetWorth (row 0, col 0) -> ShovelIntelligence (row 1, col 0).
// ---------------------------------------------------------------------------
TEST(FocusController, HjklDown_SameColumn) {
    FocusController fc;
    TabNTimes(fc, 1);  // NetWorth.
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);

    EXPECT_TRUE(fc.handle_key(Event::Character('j')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelIntelligence);

    // k from ShovelIntel back up to NetWorth.
    EXPECT_TRUE(fc.handle_key(Event::Character('k')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// 8. j/Down into an empty cell falls back to the leftmost populated cell
//    in the target row.  From SyncStatus (row 0, col 2), Down lands in
//    row 1 col 2 (empty) -> falls back to ShovelIntelligence (row 1, col 0).
//
//    Q8 resolution: leftmost-populated fallback.  Documented in
//    FocusController.cpp's file header.
// ---------------------------------------------------------------------------
TEST(FocusController, HjklDownIntoEmptyCell_FallsBack) {
    FocusController fc;
    TabNTimes(fc, 3);  // NetWorth -> ShovelScore -> SyncStatus.
    ASSERT_EQ(fc.focused_widget(), WidgetId::SyncStatus);

    EXPECT_TRUE(fc.handle_key(Event::Character('j')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::ShovelIntelligence);
}

// ---------------------------------------------------------------------------
// 9. Esc from any Widget level returns to Dashboard / None.
// ---------------------------------------------------------------------------
TEST(FocusController, EscFromWidget_ReturnsToDashboard) {
    FocusController fc;
    TabNTimes(fc, 2);  // Land on ShovelScore.
    ASSERT_EQ(fc.focused_widget(), WidgetId::ShovelScore);

    EXPECT_TRUE(fc.handle_key(Event::Escape));
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
}

// ---------------------------------------------------------------------------
// 10. Esc at Dashboard level is an explicit no-op (Q3 resolved).  The
//     controller declines to consume the event so the App can route it
//     elsewhere if it ever wants to (today: nowhere — legacy Esc-exits
//     is being removed once the App is wired in commit 4).
// ---------------------------------------------------------------------------
TEST(FocusController, EscAtDashboard_IsNoOp) {
    FocusController fc;
    EXPECT_FALSE(fc.handle_key(Event::Escape));
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
}

// ---------------------------------------------------------------------------
// 11. reset() restores the initial state from any focused widget.
// ---------------------------------------------------------------------------
TEST(FocusController, Reset_RestoresInitialState) {
    FocusController fc;
    TabNTimes(fc, 4);
    ASSERT_EQ(fc.focused_widget(), WidgetId::ShovelIntelligence);

    fc.reset();
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
}

// ---------------------------------------------------------------------------
// 12. is_widget_focused truth table — for each widget id, the predicate
//     is true iff that widget is the currently focused one.
// ---------------------------------------------------------------------------
TEST(FocusController, IsWidgetFocused_TruthTable) {
    const WidgetId ids[] = {
        WidgetId::NetWorth,
        WidgetId::ShovelScore,
        WidgetId::SyncStatus,
        WidgetId::ShovelIntelligence,
        WidgetId::CategoryTrends,
    };

    // At Dashboard level, every predicate is false.
    {
        FocusController fc;
        for (auto w : ids) {
            EXPECT_FALSE(fc.is_widget_focused(w))
                << "Dashboard-level should not report any widget focused";
        }
        // WidgetId::None must always return false even at Widget level.
        EXPECT_FALSE(fc.is_widget_focused(WidgetId::None));
    }

    // For each focused widget, exactly that one predicate is true.
    for (std::size_t target = 0; target < std::size(ids); ++target) {
        FocusController fc;
        TabNTimes(fc, static_cast<int>(target + 1));
        ASSERT_EQ(fc.focused_widget(), ids[target]);
        for (std::size_t i = 0; i < std::size(ids); ++i) {
            const bool expected = (i == target);
            EXPECT_EQ(fc.is_widget_focused(ids[i]), expected)
                << "target=" << target << " i=" << i;
        }
        // Sanity: None is never reported as focused.
        EXPECT_FALSE(fc.is_widget_focused(WidgetId::None));
    }
}
