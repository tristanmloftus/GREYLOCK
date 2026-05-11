// test_widgets_snapshot.cpp — Phase 5 TUI snapshot regression suite.
//
// Each test renders a single widget at 80x24 with frozen, whole-cent
// inputs and compares the result to a checked-in fixture under
// tests/snapshot/fixtures/.
//
// To capture or refresh fixtures locally:
//   TF_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R WidgetSnapshot
// then commit the resulting fixtures/*.txt files.

#include "snapshot/snapshot_helper.h"

#include <gtest/gtest.h>

#include "../src/views/widgets/consolidation_ui.h"
#include "../src/views/widgets/ui_category_trends.h"
#include "../src/views/widgets/ui_net_worth.h"
#include "../src/views/widgets/ui_shovel_intelligence.h"
#include "../src/views/widgets/ui_shovel_score.h"
#include "../src/views/widgets/ui_sync_status.h"
#include "../src/views/widgets/ui_updates.h"

namespace {

constexpr int W = 80;
constexpr int H = 24;

}  // namespace

TEST(WidgetSnapshot, NetWorthPositive) {
    auto element = ftxui::NetWorthBreakdownRenderer(
        /*checking=*/1234.56,
        /*savings=*/2500.00,
        /*credit=*/-500.00,
        /*investment=*/10000.00,
        /*net_worth=*/13234.56);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "net_worth_positive");
}

TEST(WidgetSnapshot, NetWorthNegative) {
    auto element = ftxui::NetWorthBreakdownRenderer(
        /*checking=*/100.00,
        /*savings=*/0.00,
        /*credit=*/-15000.00,
        /*investment=*/0.00,
        /*net_worth=*/-14900.00);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "net_worth_negative");
}

TEST(WidgetSnapshot, CategoryTrends) {
    std::vector<tf::widgets::CategoryTrend> trends = {
        {/*name=*/"Food & Dining",  /*emoji=*/"[food]", /*current=*/420.50, /*pct=*/15.0},
        {/*name=*/"Transportation", /*emoji=*/"[trsp]", /*current=*/180.00, /*pct=*/-25.0},
        {/*name=*/"Shopping",       /*emoji=*/"[shop]", /*current=*/600.00, /*pct=*/0.0},
        {/*name=*/"Entertainment",  /*emoji=*/"[ent ]", /*current=*/75.00,  /*pct=*/50.0},
        {/*name=*/"Utilities",      /*emoji=*/"[util]", /*current=*/220.00, /*pct=*/-5.0},
    };
    auto element = ftxui::CategorySpendingTrendsRenderer(trends, /*max_items=*/5);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "category_trends");
}

TEST(WidgetSnapshot, ShovelIntelligence) {
    std::vector<tf::widgets::SupplierSpend> suppliers = {
        {/*ticker=*/"NVDA", /*company=*/"NVIDIA Corp",   /*amount=*/2500.00, /*pct=*/120.0},
        {/*ticker=*/"AMZN", /*company=*/"Amazon",        /*amount=*/1800.00, /*pct=*/15.0},
        {/*ticker=*/"MSFT", /*company=*/"Microsoft",     /*amount=*/950.00,  /*pct=*/-10.0},
        {/*ticker=*/"GOOGL",/*company=*/"Alphabet",      /*amount=*/640.00,  /*pct=*/0.0},
    };
    auto element = ftxui::ShovelIntelligenceRenderer(suppliers);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "shovel_intelligence");
}

TEST(WidgetSnapshot, ShovelScore) {
    auto element = ftxui::ShovelScoreRenderer(
        /*score=*/72.0,
        /*supplier_count=*/4,
        /*total_shovel_spend=*/5890.00);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "shovel_score");
}

TEST(WidgetSnapshot, SyncStatus) {
    std::vector<tf::widgets::SyncStatus> statuses = {
        {/*institution=*/"Chase",            /*connected=*/true,  /*last_sync=*/"2026-04-01", /*err=*/""},
        {/*institution=*/"Bank of America",  /*connected=*/false, /*last_sync=*/"",            /*err=*/"auth failed"},
        {/*institution=*/"Fidelity",         /*connected=*/true,  /*last_sync=*/"2026-03-28", /*err=*/""},
    };
    auto element = ftxui::SyncStatusIndicatorRenderer(statuses);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "sync_status");
}

TEST(WidgetSnapshot, Updates) {
    std::vector<std::pair<std::string, std::string>> suppliers = {
        {"NVDA",  "GPU infrastructure"},
        {"AMZN",  "AWS compute"},
        {"MSFT",  "Azure compute"},
    };
    auto element = ftxui::SupplierTickerDisplayRenderer(suppliers);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "updates");
}

TEST(WidgetSnapshot, ConsolidationUi) {
    std::vector<tf::widgets::AccountConnection> accounts = {
        {/*inst=*/"Chase",   /*name=*/"Checking ...1234", /*bal=*/1234.56, /*conn=*/true,  /*sync=*/"2026-04-01"},
        {/*inst=*/"Fidelity",/*name=*/"Brokerage ...9999",/*bal=*/10000.00,/*conn=*/true,  /*sync=*/"2026-03-28"},
    };
    auto element = ftxui::BankConnectionStatusRenderer(accounts);
    tf::snapshot::ExpectMatchesFixture(element, W, H, "consolidation_ui");
}
