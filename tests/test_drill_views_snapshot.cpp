// ---------------------------------------------------------------------------
// test_drill_views_snapshot.cpp — Task v0.3-2.
// ---------------------------------------------------------------------------
// Snapshot tests for the two drill views landed in this task:
//
//   1. Drill_NetWorth      -> tests/snapshot/fixtures/drill_net_worth.txt
//   2. Drill_ShovelScore   -> tests/snapshot/fixtures/drill_shovel_score.txt
//
// Both fixtures are 120x40, the canvas size documented in §3b of
// docs/UI_REDESIGN_V0.3.md.  Run with TF_UPDATE_SNAPSHOTS=1 to capture.
//
// The DataStore for each test is populated by hand with deterministic
// inputs that exercise the non-empty render paths (the empty-state
// paths are guarded by branches in the drill code; widget-snapshot
// tests will pick those up in v0.3-3 when an "empty fixtures" pass is
// added).
// ---------------------------------------------------------------------------

#include "snapshot/snapshot_helper.h"

#include <gtest/gtest.h>

#include "../src/models/Account.h"
#include "../src/models/DataStore.h"
#include "../src/models/Entity.h"
#include "../src/models/Transaction.h"
#include "../src/views/drills/Drill_NetWorth.h"
#include "../src/views/drills/Drill_ShovelScore.h"

namespace {

constexpr int W = 120;
constexpr int H = 40;

// Build the same in-memory DataStore the dashboard_net_worth_focused
// fixture uses, plus a handful of transactions so the per-account
// last-sync column has data to render.  Keeping the data in one helper
// makes the two test cases share intent: same accounts, same entity,
// different drill.
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
        a.id          = id;       // deterministic; mask shows last-4.
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

    // Per-account last-sync stamps come from newest tx.date on each
    // account.  We seed one tx per account so every row has a date.
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

// Seed a DataStore with merchant-mapped transactions so the Shovel
// Score drill has inputs to score.  Uses descriptions the hardcoded
// fallback ruleset recognises (NVIDIA, AMAZON, MICROSOFT, GOOGLE CLOUD)
// so the test is independent of data/supplier_map.json content.
void seed_shovel_dataset(DataStore& data_store) {
    Entity entity;
    entity.id   = "ent-1";
    entity.name = "Personal";
    entity.type = EntityType::Individual;
    data_store.add_entity(entity);

    Account a;
    a.id          = "acct-shovel";
    a.entity_id   = "ent-1";
    a.name        = "Brokerage";
    a.type        = AccountType::Investment;
    a.balance     = 10000.00;
    a.institution = "Fidelity";
    data_store.add_account(a);

    auto make_tx = [&](const std::string& date, double amount,
                       const std::string& desc) {
        Transaction t;
        t.id          = "tx-" + desc.substr(0, 4) + "-" + date;
        t.account_id  = "acct-shovel";
        t.date        = date;
        t.amount      = amount;
        t.description = desc;
        data_store.transactions.push_back(t);
    };
    // NVDA: cur 1200, prev 545 -> +120.18%
    make_tx("2026-05-15", -1200.00, "NVIDIA");
    make_tx("2026-04-10",  -545.00, "NVIDIA");
    // AMZN: cur 900, prev 783 -> +14.94%
    make_tx("2026-05-02",  -900.00, "AMZN MKTPLACE");
    make_tx("2026-04-01",  -783.00, "AMAZON.COM");
    // MSFT: cur 310, prev 345 -> -10.14%
    make_tx("2026-05-05",  -310.00, "MICROSOFT");
    make_tx("2026-04-05",  -345.00, "MICROSOFT");
    // GOOGL: cur 200, prev 200 -> 0%
    make_tx("2026-05-06",  -200.00, "GOOGLE CLOUD");
    make_tx("2026-04-06",  -200.00, "GOOGLE CLOUD");
}

}  // namespace

TEST(DrillViewsSnapshot, NetWorth) {
    DataStore data_store;
    seed_dashboard_dataset(data_store);

    tf::views::drills::Drill_NetWorth drill(data_store, "ent-1");
    auto element = drill.render();

    tf::snapshot::ExpectMatchesFixture(element, W, H, "drill_net_worth");
}

TEST(DrillViewsSnapshot, ShovelScore) {
    DataStore data_store;
    seed_shovel_dataset(data_store);

    tf::views::drills::Drill_ShovelScore drill(
        data_store, "2026-05", "2026-04");
    auto element = drill.render();

    tf::snapshot::ExpectMatchesFixture(element, W, H, "drill_shovel_score");
}
