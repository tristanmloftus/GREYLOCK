// test_focus_controller.cpp — unit tests for the Dashboard focus state
// machine.  Pure state-machine tests; no FTXUI rendering is involved.
// See src/views/FocusController.h for the contract being exercised here.
//
// 2026-05-16: tests rewritten for the 4-widget post-shovel-scrub grid
//   row 0:  NetWorth         CashFlow
//   row 1:  RecentActivity   SyncStatus

#include "../src/views/FocusController.h"

#include <ftxui/component/event.hpp>

#include <gtest/gtest.h>

using tf::views::FocusController;
using tf::views::FocusLevel;
using tf::views::WidgetId;
using ftxui::Event;

// ---------------------------------------------------------------------------
// Initial state — Dashboard level, no widget focused.
// ---------------------------------------------------------------------------
TEST(FocusController, InitialState_DashboardLevelNoWidget) {
    FocusController fc;
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
    EXPECT_FALSE(fc.is_widget_focused(WidgetId::NetWorth));
}

// ---------------------------------------------------------------------------
// Tab from Dashboard lands on NetWorth.
// ---------------------------------------------------------------------------
TEST(FocusController, TabFromDashboard_LandsOnFirstWidget) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.level(), FocusLevel::Widget);
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// Tab cycles in declaration order through all four widgets, then wraps.
// ---------------------------------------------------------------------------
TEST(FocusController, TabCyclesAndWraps) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::CashFlow);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::RecentActivity);
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
    // Wrap.
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// Shift-Tab from Dashboard lands on the last widget in order (SyncStatus).
// ---------------------------------------------------------------------------
TEST(FocusController, ShiftTabFromDashboard_LandsOnLastWidget) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::TabReverse));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
}

// ---------------------------------------------------------------------------
// h/Right wrap within row 0:  NetWorth -> CashFlow -> NetWorth.
// ---------------------------------------------------------------------------
TEST(FocusController, RightOnRow0_WrapsWithinRow) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    EXPECT_TRUE(fc.handle_key(Event::Character('l')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::CashFlow);
    EXPECT_TRUE(fc.handle_key(Event::Character('l')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// j/Down from NetWorth (row 0, col 0) lands on RecentActivity (row 1, col 0).
// ---------------------------------------------------------------------------
TEST(FocusController, DownFromNetWorth_LandsOnRecentActivity) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    EXPECT_TRUE(fc.handle_key(Event::Character('j')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::RecentActivity);
    EXPECT_TRUE(fc.handle_key(Event::Character('k')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}

// ---------------------------------------------------------------------------
// j/Down from CashFlow (row 0, col 1) lands on SyncStatus (row 1, col 1).
// All cells populated, no fallback path exercised.
// ---------------------------------------------------------------------------
TEST(FocusController, DownFromCashFlow_LandsOnSyncStatus) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));            // NetWorth
    EXPECT_TRUE(fc.handle_key(Event::Tab));            // CashFlow
    ASSERT_EQ(fc.focused_widget(), WidgetId::CashFlow);
    EXPECT_TRUE(fc.handle_key(Event::Character('j')));
    EXPECT_EQ(fc.focused_widget(), WidgetId::SyncStatus);
}

// ---------------------------------------------------------------------------
// Esc on Widget level pops to Dashboard.
// ---------------------------------------------------------------------------
TEST(FocusController, EscOnWidget_PopsToDashboard) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    ASSERT_EQ(fc.level(), FocusLevel::Widget);
    EXPECT_TRUE(fc.handle_key(Event::Escape));
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
}

// ---------------------------------------------------------------------------
// reset() restores Dashboard/None from any state.
// ---------------------------------------------------------------------------
TEST(FocusController, Reset_RestoresDashboardNone) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    ASSERT_EQ(fc.focused_widget(), WidgetId::CashFlow);
    fc.reset();
    EXPECT_EQ(fc.level(), FocusLevel::Dashboard);
    EXPECT_EQ(fc.focused_widget(), WidgetId::None);
}

// ---------------------------------------------------------------------------
// enter_drill / exit_drill round-trip on NetWorth.
// ---------------------------------------------------------------------------
TEST(FocusController, DrillRoundTrip_NetWorth) {
    FocusController fc;
    EXPECT_TRUE(fc.handle_key(Event::Tab));
    ASSERT_EQ(fc.focused_widget(), WidgetId::NetWorth);
    EXPECT_TRUE(fc.enter_drill(WidgetId::NetWorth));
    EXPECT_EQ(fc.level(), FocusLevel::Drill);
    EXPECT_EQ(fc.drilled_widget(), WidgetId::NetWorth);
    EXPECT_TRUE(fc.handle_key(Event::Escape));
    EXPECT_EQ(fc.level(), FocusLevel::Widget);
    EXPECT_EQ(fc.focused_widget(), WidgetId::NetWorth);
}
