// ---------------------------------------------------------------------------
// test_drill_views_snapshot.cpp — drill-view snapshot regression.
// ---------------------------------------------------------------------------
// Snapshot test for Drill_NetWorth at 120x40 (the canvas size documented
// in docs/UI_REDESIGN_V0.3.md §3b).  Run with TF_UPDATE_SNAPSHOTS=1 to
// capture.
//
// The DataStore is populated by hand with deterministic inputs that
// exercise the non-empty render paths.
// ---------------------------------------------------------------------------

#include "snapshot/snapshot_helper.h"

#include <gtest/gtest.h>

#include "../src/models/Account.h"
#include "../src/models/DataStore.h"
#include "../src/models/Entity.h"
#include "../src/models/Transaction.h"
#include "../src/views/drills/Drill_NetWorth.h"

namespace {

constexpr int W = 120;
constexpr int H = 40;

// Same in-memory DataStore the dashboard_net_worth_focused fixture uses,
// plus a handful of transactions so the per-account last-sync column
// has data to render.
void seed_dashboard_dataset(DataStore& data_store) {
    Entity entity;
    entity.id   = "ent-1";
    entity.name = "Personal";
    entity.type = EntityType::Individual;
    data_store.add_entity(entity);

    auto make_account = [&](AccountType type,
                            const std::string& id,
                            const std::string& name,
                            double balance,
                            const std::string& institution) {
        Account a;
        a.id          = id;
        a.entity_id   = "ent-1";
        a.name        = name;
        a.type        = type;
        a.balance     = balance;
        a.institution = institution;
        data_store.add_account(a);
    };
    make_account(AccountType::Checking,   "acct-1234", "Chase Checking",  1234.56, "Chase");
    make_account(AccountType::Savings,    "acct-5678", "Wells Savings",   2500.00, "Wells Fargo");
    make_account(AccountType::CreditCard, "acct-9012", "Chase Sapphire",  -500.00, "Chase");
    make_account(AccountType::Investment, "acct-9999", "Fidelity Brokr", 10000.00, "Fidelity");

    auto make_tx = [&](const std::string& acct, const std::string& date,
                       double amount, const std::string& desc) {
        Transaction t;
        t.id          = "tx-" + acct + "-" + date;
        t.account_id  = acct;
        t.date        = date;
        t.amount      = amount;
        t.description = desc;
        data_store.transactions.push_back(t);
    };
    make_tx("acct-1234", "2026-04-01",  -50.00, "Coffee");
    make_tx("acct-5678", "2026-04-01",  100.00, "Interest");
    make_tx("acct-9012", "2026-04-01", -200.00, "Travel");
    make_tx("acct-9999", "2026-03-28",  500.00, "Dividend");
}

}  // namespace

TEST(DrillViewsSnapshot, NetWorth) {
    DataStore data_store;
    seed_dashboard_dataset(data_store);

    tf::views::drills::Drill_NetWorth drill(data_store, "ent-1");
    auto element = drill.render();

    tf::snapshot::ExpectMatchesFixture(element, W, H, "drill_net_worth");
}
