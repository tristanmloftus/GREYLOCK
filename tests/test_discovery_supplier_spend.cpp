// ---------------------------------------------------------------------------
// test_discovery_supplier_spend.cpp — Task v0.3-2.
// ---------------------------------------------------------------------------
// Locks in the behavior of DiscoveryService::aggregate_supplier_spend(),
// the per-supplier aggregator extracted from DashboardView.cpp (pre-
// c0fd7db inline lines ~322-355) so the Dashboard widget and the Shovel-
// Score drill view render off the same numbers.
//
// THE FIVE CASES BELOW DOCUMENT THE CONTRACT
//   1. Realistic merchant strings map to the expected tickers (the
//      hardcoded fallback ruleset is sufficient — no JSON fixture
//      required, matching the test_discovery_merchant_velocity pattern).
//   2. MoM percent_change uses the same first-month sentinel as
//      DashboardView's inline version (100% from a zero baseline; 0%
//      when both months are zero).
//   3. Output is sorted DESC by total_spend with an alphabetical ticker
//      tie-breaker.
//   4. An empty transaction vector yields an empty result vector (no
//      crash, no synthesized rows).
//   5. Income (positive amounts) is excluded; only expenses count.
//
// HOW THIS GUARDS THE DASHBOARDVIEW REFACTOR
//   The Task v0.3-2 brief asked for byte-identical DashboardView snapshots
//   after the refactor.  DashboardFocusSnapshotTests provides that
//   regression guard at the visual level.  This file provides the unit-
//   level guard: if anyone changes the aggregator semantics, these
//   assertions fail before the snapshot does.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../src/models/Transaction.h"
#include "../src/services/DiscoveryService.h"

namespace {

// Small constructor helper to keep the test bodies short and readable.
Transaction make_tx(const std::string& date,
                    double amount,
                    const std::string& description) {
    Transaction t;
    t.id          = "tx-" + description.substr(0, std::min<size_t>(description.size(), 8));
    t.account_id  = "acct-1";
    t.date        = date;
    t.amount      = amount;
    t.description = description;
    return t;
}

// Locate a row by ticker.  Returns nullptr if absent so callers can
// EXPECT_EQ on the count first before dereferencing.
const tf::services::SupplierSpend* find_ticker(
    const std::vector<tf::services::SupplierSpend>& rows,
    const std::string& ticker) {
    for (const auto& r : rows) {
        if (r.ticker == ticker) return &r;
    }
    return nullptr;
}

}  // namespace

TEST(DiscoverySupplierSpend, MapsRealisticMerchantsToCorrectTickers) {
    // Spread the same NVIDIA buy across two real-world description
    // variants so we exercise the description -> ticker step plus the
    // per-ticker aggregation (variants must collapse to one row).
    std::vector<Transaction> txs = {
        make_tx("2026-05-15", -1200.00, "NVIDIA CORP CUST# 12345"),
        make_tx("2026-05-20",  -300.00, "NVIDIA"),
        make_tx("2026-04-10",  -545.00, "NVIDIA"),
        make_tx("2026-05-02",  -900.00, "AMZN MKTPLACE PMTS"),
        make_tx("2026-04-01",  -783.00, "AMAZON.COM ORDER"),
    };

    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");

    // Both variants of the same supplier should collapse to one row.
    ASSERT_EQ(rows.size(), 2u);

    const auto* nvda = find_ticker(rows, "NVDA");
    ASSERT_NE(nvda, nullptr);
    EXPECT_DOUBLE_EQ(nvda->total_spend,    2045.00);  // 1200 + 300 + 545
    EXPECT_DOUBLE_EQ(nvda->current_spend,  1500.00);  // 1200 + 300
    EXPECT_DOUBLE_EQ(nvda->previous_spend,  545.00);

    const auto* amzn = find_ticker(rows, "AMZN");
    ASSERT_NE(amzn, nullptr);
    EXPECT_DOUBLE_EQ(amzn->total_spend,    1683.00);  // 900 + 783
    EXPECT_DOUBLE_EQ(amzn->current_spend,   900.00);
    EXPECT_DOUBLE_EQ(amzn->previous_spend,  783.00);
}

TEST(DiscoverySupplierSpend, ComputesMoMPercentChangeCorrectly) {
    // Three regimes:
    //   - Ticker A: prev > 0  -> standard ((cur - prev)/prev)*100.
    //   - Ticker B: prev == 0, cur > 0 -> the "100%" first-month sentinel.
    //   - Ticker C: prev > 0, cur == 0 -> -100% (full evaporation).
    std::vector<Transaction> txs = {
        // NVDA: cur 1200, prev 545 -> +120.18%
        make_tx("2026-05-01", -1200.00, "NVIDIA"),
        make_tx("2026-04-01",  -545.00, "NVIDIA"),
        // AMZN: cur 900, prev 0 -> +100% sentinel.
        make_tx("2026-05-01",  -900.00, "AMAZON"),
        // MSFT: cur 0, prev 1000 -> -100%.
        make_tx("2026-04-15", -1000.00, "MICROSOFT"),
    };

    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");

    ASSERT_EQ(rows.size(), 3u);

    const auto* nvda = find_ticker(rows, "NVDA");
    ASSERT_NE(nvda, nullptr);
    EXPECT_NEAR(nvda->percent_change, ((1200.0 - 545.0) / 545.0) * 100.0, 1e-9);

    const auto* amzn = find_ticker(rows, "AMZN");
    ASSERT_NE(amzn, nullptr);
    EXPECT_DOUBLE_EQ(amzn->percent_change, 100.0);  // first-month sentinel

    const auto* msft = find_ticker(rows, "MSFT");
    ASSERT_NE(msft, nullptr);
    EXPECT_DOUBLE_EQ(msft->percent_change, -100.0);
}

TEST(DiscoverySupplierSpend, ReturnsRowsSortedByDescendingSpend) {
    // Four tickers with strictly descending total_spend so the sort is
    // unambiguous.  We do NOT exercise the tie-breaker here — that lives
    // in its own dedicated case below.
    std::vector<Transaction> txs = {
        make_tx("2026-05-01",  -100.00, "GOOGLE CLOUD"),  // GOOGL -> 100
        make_tx("2026-05-01",  -500.00, "AMAZON"),        // AMZN  -> 500
        make_tx("2026-05-01", -1000.00, "MICROSOFT"),     // MSFT  -> 1000
        make_tx("2026-05-01", -2000.00, "NVIDIA"),        // NVDA  -> 2000
    };

    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");

    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].ticker, "NVDA");
    EXPECT_EQ(rows[1].ticker, "MSFT");
    EXPECT_EQ(rows[2].ticker, "AMZN");
    EXPECT_EQ(rows[3].ticker, "GOOGL");
    EXPECT_DOUBLE_EQ(rows[0].total_spend, 2000.0);
    EXPECT_DOUBLE_EQ(rows[3].total_spend,  100.0);
}

TEST(DiscoverySupplierSpend, SortBreaksTiesAlphabeticallyByTicker) {
    // Two tickers with the SAME total_spend.  The aggregator's contract
    // says ties break alphabetically (AAPL before MSFT here).
    std::vector<Transaction> txs = {
        make_tx("2026-05-01",  -500.00, "MICROSOFT"),  // MSFT
        make_tx("2026-05-01",  -500.00, "APPLE"),      // AAPL
    };

    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].ticker, "AAPL");
    EXPECT_EQ(rows[1].ticker, "MSFT");
}

TEST(DiscoverySupplierSpend, HandlesEmptyTransactionsGracefully) {
    std::vector<Transaction> txs;  // no transactions at all
    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");
    EXPECT_TRUE(rows.empty());
}

TEST(DiscoverySupplierSpend, ExcludesIncomeAndUnknownDescriptions) {
    std::vector<Transaction> txs = {
        // Income: positive amount, even though the description matches.
        make_tx("2026-05-01", +5000.00, "NVIDIA PAYROLL"),
        // Unknown merchant: no rule matches "MOM AND POP DINER".
        make_tx("2026-05-01",  -200.00, "MOM AND POP DINER"),
        // Real expense: should be the only row.
        make_tx("2026-05-01",  -100.00, "AMAZON"),
    };

    auto rows = DiscoveryService::instance().aggregate_supplier_spend(
        txs, "2026-05", "2026-04");

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].ticker, "AMZN");
    EXPECT_DOUBLE_EQ(rows[0].total_spend, 100.0);
}
