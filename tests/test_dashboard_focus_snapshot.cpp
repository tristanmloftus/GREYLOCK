// test_dashboard_focus_snapshot.cpp — Task v0.3-1 end-to-end Dashboard
// snapshot with FocusController-driven panel highlighting.
//
// Renders the full DashboardView at 120x40 with the FocusController
// pointed at NetWorth, and locks the result against the checked-in
// fixture tests/snapshot/fixtures/dashboard_net_worth_focused.txt.
//
// The DataStore is populated by hand with a single entity, four
// accounts (one of each type), and zero transactions.  CategoryTrends
// renders the "No transactions this month" placeholder; SyncStatus
// renders one row per institution.  Net worth, the only widget driven
// by accounts (not transactions), gets meaningful data.
//
// Run with TF_UPDATE_SNAPSHOTS=1 to capture the fixture after a code
// change that intentionally alters Dashboard output; the matching
// commit ships the regenerated fixture alongside the source change.

#include "snapshot/snapshot_helper.h"

#include <gtest/gtest.h>

#include <ftxui/component/event.hpp>

#include "../src/models/Account.h"
#include "../src/models/DataStore.h"
#include "../src/models/Entity.h"
#include "../src/views/DashboardView.h"
#include "../src/views/FocusController.h"

namespace {

// Dashboard composes three panels in a 2x2 grid.  We size the snapshot
// canvas generously (120x40) to give every panel room without
// clipping; the same dimensions are documented in the redesign proposal
// drill mockups.
constexpr int W = 120;
constexpr int H = 40;

}  // namespace

TEST(DashboardFocusSnapshot, NetWorthFocused) {
    // ----- Set up a deterministic in-memory DataStore.  No storage
    //       backend is attached (set_storage is unset), so save/load
    //       are no-ops and the test stays read-only.
    DataStore data_store;

    Entity entity;
    entity.id   = "ent-1";
    entity.name = "Personal";
    entity.type = EntityType::Individual;
    data_store.add_entity(entity);

    auto make_account = [&](AccountType type, const std::string& name,
                            double balance) {
        Account a;
        a.id          = name;  // deterministic ID so render is stable.
        a.entity_id   = "ent-1";
        a.name        = name;
        a.type        = type;
        a.balance     = balance;
        a.institution = "Test Bank";
        data_store.add_account(a);
    };
    make_account(AccountType::Checking,   "Checking",   1234.56);
    make_account(AccountType::Savings,    "Savings",    2500.00);
    make_account(AccountType::CreditCard, "CreditCard", -500.00);
    make_account(AccountType::Investment, "Investment", 10000.00);

    // No transactions: CategoryTrends renders the "No transactions"
    // placeholder, SyncStatus renders one row per institution with
    // last_sync = "".

    // ----- Drive the FocusController to Widget(NetWorth).
    tf::views::FocusController focus;
    ASSERT_TRUE(focus.handle_key(ftxui::Event::Tab));
    ASSERT_EQ(focus.focused_widget(), tf::views::WidgetId::NetWorth);

    // ----- Render.
    DashboardView view(data_store);
    view.set_entity_id("ent-1");
    auto element = view.render("2026-05", &focus);

    tf::snapshot::ExpectMatchesFixture(element, W, H, "dashboard_net_worth_focused");
}
