// tests/test_discovery_merchant_velocity.cpp — Phase 5 discovery tests.
//
// Two test surfaces, both unit-level (no server, no DB):
//
//  1. Merchant-string -> supplier/ticker mapping against the realistic
//     messy descriptions the v0.2 plan calls out:
//       STARBUCKSUS123, STARBUCKS STORE #0042, AMZN MKTPLACE * SUBSCRIBE,
//       AMAZON.COM*MK7Y2X, COSTCO WHSE #0123, USPS PO 12345, WALMART.COM,
//       TGT *T1234, APPLE.COM/BILL, NETFLIX.COM.
//
//  2. MoM velocity formula assertions over 6 months of synthetic
//     transactions per supplier.
//
// Velocity formula (current implementation):
//
//     percent_change = ((current_month - previous_month) / previous_month) * 100
//
//   Edge cases:
//     - previous_month == 0 && current_month  > 0 -> sentinel 100.0
//     - previous_month == 0 && current_month == 0 -> entry omitted from results
//     - current_month  == 0 && previous_month > 0 -> percent_change == -100
//       (dropped to zero; the previous-month spend went to zero this month)
//
// Tests use calculateVelocityForMonths(...) (the deterministic variant)
// so results never depend on wall-clock time.

#include <gtest/gtest.h>

#include "src/services/DiscoveryService.h"
#include "src/models/Transaction.h"
#include "src/models/Category.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef TF_SUPPLIER_MAP_FIXTURE
#  define TF_SUPPLIER_MAP_FIXTURE "data/supplier_map.json"
#endif

namespace {

// Load the canonical JSON fixture into the singleton.  The constructor's
// best-effort load() reads data/supplier_map.json relative to CWD which
// is the build dir at test time — point it at the source-tree path
// instead via the absolute fixture macro.
void ensure_canonical_rules_loaded() {
    bool ok = DiscoveryService::instance().load_from_json(TF_SUPPLIER_MAP_FIXTURE);
    if (!ok) {
        // Fixture missing on a stripped-down build — fall back to the
        // hardcoded rule set so the test still exercises the matching
        // logic.  The integration test enforces that the fixture exists.
        DiscoveryService::instance().initializeSupplierMap();
    }
}

struct MerchantCase {
    std::string description;
    std::string expected_ticker;
    std::string expected_supplier_contains; // substring match on supplier name
};

// Helper: build a single Transaction with a YYYY-MM-DD date.
Transaction tx(const std::string& date, double amount, const std::string& category) {
    Transaction t;
    t.date        = date;
    t.amount      = amount;
    t.category_id = category;
    return t;
}

} // namespace

// --------------------------------------------------------------------------
// 1. Merchant-string mapping
// --------------------------------------------------------------------------

TEST(DiscoveryMerchant, StarbucksUS123_ResolvesToSBUX) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("STARBUCKSUS123");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "SBUX");
    EXPECT_NE(info->supplier.find("Starbucks"), std::string::npos);
}

TEST(DiscoveryMerchant, StarbucksStoreWithNumber_ResolvesToSBUX) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("STARBUCKS STORE #0042");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "SBUX");
}

TEST(DiscoveryMerchant, AmznMktplaceSubscribe_ResolvesToAMZN) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("AMZN MKTPLACE * SUBSCRIBE");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "AMZN");
    EXPECT_NE(info->supplier.find("Amazon"), std::string::npos);
}

TEST(DiscoveryMerchant, AmazonComOrderToken_ResolvesToAMZN) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("AMAZON.COM*MK7Y2X");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "AMZN");
}

TEST(DiscoveryMerchant, CostcoWhseWithNumber_ResolvesToCOST) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("COSTCO WHSE #0123");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "COST");
}

TEST(DiscoveryMerchant, UspsPo_ResolvesToUSPS_WithEmptyTicker) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("USPS PO 12345");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->supplier, "USPS");
    EXPECT_TRUE(info->ticker.empty())
        << "USPS has no public listing; ticker must be empty (got '"
        << info->ticker << "')";

    // mapToSupplier() returns the ticker; for USPS that is the empty
    // string — but the optional itself MUST be engaged so callers can
    // distinguish "matched, no ticker" from "did not match".
    auto ticker = DiscoveryService::instance().mapToSupplier("USPS PO 12345");
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(*ticker, "");
}

TEST(DiscoveryMerchant, WalmartCom_ResolvesToWMT) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("WALMART.COM");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "WMT");
}

TEST(DiscoveryMerchant, TgtSpaceT1234_ResolvesToTGT) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("TGT *T1234");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "TGT");
}

TEST(DiscoveryMerchant, AppleComBill_ResolvesToAAPL) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("APPLE.COM/BILL");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "AAPL");
}

TEST(DiscoveryMerchant, NetflixCom_ResolvesToNFLX) {
    ensure_canonical_rules_loaded();
    auto info = DiscoveryService::instance().getSupplierInfo("NETFLIX.COM");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->ticker, "NFLX");
}

TEST(DiscoveryMerchant, UnknownDescription_ReturnsNullopt) {
    ensure_canonical_rules_loaded();
    EXPECT_FALSE(DiscoveryService::instance()
                     .getSupplierInfo("RANDOM NON MATCHING MERCHANT XYZ").has_value());
    EXPECT_FALSE(DiscoveryService::instance().mapToSupplier("").has_value());
}

// --------------------------------------------------------------------------
// 2. MoM Velocity formula
// --------------------------------------------------------------------------
//
// Build a small DataStore-like transaction set spanning Jan-Jun 2026 across
// three categories with whole-dollar amounts so we can compute expected
// percent_change exactly.
//
// Categories:
//   cat_food           - rising spend (every month, growing)
//   cat_subscriptions  - flat spend (every month, constant)
//   cat_gas            - first-month-only category (no prior spend)
//   cat_health         - drops to zero in current month (was 200 last month)

static std::vector<Transaction> build_six_month_ledger() {
    // Whole-dollar amounts. Signs negative = expense (the only thing the
    // velocity engine counts).
    std::vector<Transaction> v;
    v.push_back(tx("2026-01-15", -100.0, "cat_food"));
    v.push_back(tx("2026-02-15", -120.0, "cat_food"));
    v.push_back(tx("2026-03-15", -140.0, "cat_food"));
    v.push_back(tx("2026-04-15", -160.0, "cat_food"));
    v.push_back(tx("2026-05-15", -180.0, "cat_food"));   // previous_month
    v.push_back(tx("2026-06-15", -270.0, "cat_food"));   // current_month  → +50%

    v.push_back(tx("2026-05-01", -50.0,  "cat_subscriptions"));  // previous
    v.push_back(tx("2026-06-01", -50.0,  "cat_subscriptions"));  // current → 0%

    // First-month supplier: only appears in the current month.
    v.push_back(tx("2026-06-10", -40.0,  "cat_gas"));    // previous_month spend = 0

    // Dropped to zero this month.
    v.push_back(tx("2026-05-20", -200.0, "cat_health")); // previous_month = 200
                                                          // current_month  = 0 → -100%

    // Income transactions must be ignored entirely.
    v.push_back(tx("2026-06-20", +5000.0, "cat_salary"));
    return v;
}

TEST(DiscoveryVelocity, FormulaAgainstSixMonthLedger) {
    ensure_canonical_rules_loaded();

    auto ledger = build_six_month_ledger();
    std::vector<Category> categories = DEFAULT_CATEGORIES;

    auto results = DiscoveryService::instance().calculateVelocityForMonths(
        ledger, categories, "2026-06", "2026-05");

    // Expect entries for: cat_food, cat_subscriptions, cat_gas, cat_health.
    // (cat_salary is income → excluded.  Months Jan-Apr are excluded by
    // the prefix filter.)
    ASSERT_EQ(results.size(), 4u);

    // Index by category for easier assertion.
    std::map<std::string, VelocityResult> by_cat;
    for (const auto& r : results) by_cat[r.category_id] = r;

    ASSERT_TRUE(by_cat.count("cat_food"));
    EXPECT_DOUBLE_EQ(by_cat["cat_food"].previous_month_spend, 180.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_food"].current_month_spend,  270.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_food"].percent_change,       50.0);

    ASSERT_TRUE(by_cat.count("cat_subscriptions"));
    EXPECT_DOUBLE_EQ(by_cat["cat_subscriptions"].previous_month_spend, 50.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_subscriptions"].current_month_spend,  50.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_subscriptions"].percent_change,        0.0);

    ASSERT_TRUE(by_cat.count("cat_gas"));
    EXPECT_DOUBLE_EQ(by_cat["cat_gas"].previous_month_spend,  0.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_gas"].current_month_spend,  40.0);
    // First-month sentinel: 100.0.
    EXPECT_DOUBLE_EQ(by_cat["cat_gas"].percent_change,      100.0);

    ASSERT_TRUE(by_cat.count("cat_health"));
    EXPECT_DOUBLE_EQ(by_cat["cat_health"].previous_month_spend, 200.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_health"].current_month_spend,    0.0);
    EXPECT_DOUBLE_EQ(by_cat["cat_health"].percent_change,      -100.0);
}

TEST(DiscoveryVelocity, SingleTransactionSupplier_NoPriorMonth) {
    ensure_canonical_rules_loaded();

    std::vector<Transaction> v;
    v.push_back(tx("2026-06-15", -42.0, "cat_food"));

    auto results = DiscoveryService::instance().calculateVelocityForMonths(
        v, DEFAULT_CATEGORIES, "2026-06", "2026-05");

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].category_id, "cat_food");
    EXPECT_DOUBLE_EQ(results[0].previous_month_spend, 0.0);
    EXPECT_DOUBLE_EQ(results[0].current_month_spend, 42.0);
    EXPECT_DOUBLE_EQ(results[0].percent_change,    100.0);
}

TEST(DiscoveryVelocity, ZeroSpendBothMonths_NotInResults) {
    ensure_canonical_rules_loaded();

    // Empty ledger -> empty results.
    auto results = DiscoveryService::instance().calculateVelocityForMonths(
        {}, DEFAULT_CATEGORIES, "2026-06", "2026-05");
    EXPECT_TRUE(results.empty());
}

TEST(DiscoveryVelocity, IncomeIsIgnored) {
    ensure_canonical_rules_loaded();

    std::vector<Transaction> v;
    v.push_back(tx("2026-06-01", +9999.0, "cat_salary"));
    v.push_back(tx("2026-05-01", +9999.0, "cat_salary"));

    auto results = DiscoveryService::instance().calculateVelocityForMonths(
        v, DEFAULT_CATEGORIES, "2026-06", "2026-05");
    EXPECT_TRUE(results.empty());
}

// --------------------------------------------------------------------------
// JSON loader sanity
// --------------------------------------------------------------------------

TEST(DiscoveryJsonLoader, LoadsFixtureSuccessfully) {
    EXPECT_TRUE(DiscoveryService::instance().load_from_json(TF_SUPPLIER_MAP_FIXTURE));
    EXPECT_FALSE(DiscoveryService::instance().rules().empty());
}

TEST(DiscoveryJsonLoader, MissingFile_FallsBackQuietly) {
    // load_from_json reports failure but does not mutate rules_.
    auto before_count = DiscoveryService::instance().rules().size();
    EXPECT_FALSE(DiscoveryService::instance().load_from_json("/tmp/__nope__/missing.json"));
    EXPECT_EQ(DiscoveryService::instance().rules().size(), before_count);
}

TEST(DiscoveryJsonLoader, MalformedJson_LeavesRulesUnchanged) {
    ensure_canonical_rules_loaded();
    auto before_count = DiscoveryService::instance().rules().size();

    // Write a tempfile with invalid JSON.
    auto tmpdir = std::filesystem::temp_directory_path();
    auto bad = tmpdir / "tf_bad_supplier_map.json";
    {
        std::ofstream f(bad);
        f << "{ not valid json";
    }

    EXPECT_FALSE(DiscoveryService::instance().load_from_json(bad.string()));
    EXPECT_EQ(DiscoveryService::instance().rules().size(), before_count);

    std::filesystem::remove(bad);
}

TEST(DiscoveryJsonLoader, GetCanonicalMappingJson_RoundTrips) {
    ensure_canonical_rules_loaded();
    auto j = DiscoveryService::instance().get_canonical_mapping_json();
    auto parsed = nlohmann::json::parse(j);
    ASSERT_TRUE(parsed.contains("rules"));
    ASSERT_TRUE(parsed["rules"].is_array());
    EXPECT_FALSE(parsed["rules"].empty());
    EXPECT_EQ(parsed.value("version", 0), 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
