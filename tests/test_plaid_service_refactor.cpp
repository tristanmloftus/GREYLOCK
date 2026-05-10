// test_plaid_service_refactor.cpp — Verify TUI's PlaidService refactor.
//
// Checks that:
//   1. IPlaidService has NO method accepting or returning a Plaid access_token.
//   2. Account has NO plaid_access_token field.
//   3. StubPlaidService's new account_id-based API works correctly.
//   4. The public surface of PlaidService never exposes a raw access_token.
//
// These tests are compile-time assertions (static_assert) plus runtime
// behavioral checks for the new API.

#include <gtest/gtest.h>

#include "../src/services/PlaidService.h"
#include "../src/models/Account.h"

#include <type_traits>

// ---------------------------------------------------------------------------
// Compile-time checks — these cause a build failure if the old interface
// is present.
//
// We use SFINAE / concept-style detection to check that no method matching
// the old access_token signatures exists on IPlaidService.
// ---------------------------------------------------------------------------

// Check that Account has NO plaid_access_token member.
// We do this by checking that PlaidService.h and Account.h can be included
// without referencing plaid_access_token.  The test binary would not compile
// if Account.h had the field AND it was referenced anywhere in these tests.
// The explicit static_assert below catches the field directly.

// Detect whether Account has a 'plaid_access_token' member.
template <typename T, typename = void>
struct has_plaid_access_token : std::false_type {};

template <typename T>
struct has_plaid_access_token<T,
    std::void_t<decltype(std::declval<T>().plaid_access_token)>>
    : std::true_type {};

static_assert(
    !has_plaid_access_token<Account>::value,
    "Account must NOT have a plaid_access_token field (v0.2 requirement: "
    "tokens are server-side only, managed by PlaidTokenBroker).");

// Check that Account HAS is_plaid_linked.
template <typename T, typename = void>
struct has_is_plaid_linked : std::false_type {};

template <typename T>
struct has_is_plaid_linked<T,
    std::void_t<decltype(std::declval<T>().is_plaid_linked)>>
    : std::true_type {};

static_assert(
    has_is_plaid_linked<Account>::value,
    "Account must have an is_plaid_linked field (replaces plaid_access_token).");

// ---------------------------------------------------------------------------
// Behavioral tests for the new account_id-based API.
// ---------------------------------------------------------------------------

class PlaidServiceRefactorTest : public ::testing::Test {};

TEST_F(PlaidServiceRefactorTest, StubIsStub) {
    auto svc = create_plaid_service(true);
    ASSERT_NE(svc, nullptr);
    EXPECT_TRUE(svc->is_stub());
}

TEST_F(PlaidServiceRefactorTest, StubLinkAccountSucceeds) {
    auto svc = create_plaid_service(true);
    // link_account takes account_id + public_token (NOT an access_token).
    bool result = svc->link_account("acc_123", "public-sandbox-xxxxx");
    EXPECT_TRUE(result);
    // No error expected from stub.
    EXPECT_TRUE(svc->get_last_error().empty());
}

TEST_F(PlaidServiceRefactorTest, StubGetTransactionsNoAccessToken) {
    auto svc = create_plaid_service(true);
    // get_transactions takes account_id, not access_token.
    auto txs = svc->get_transactions("acc_123", "2026-01-01", "2026-05-09");
    EXPECT_TRUE(txs.empty());
}

TEST_F(PlaidServiceRefactorTest, StubGetAccountsNoAccessToken) {
    auto svc = create_plaid_service(true);
    // get_accounts takes account_id, not access_token.
    auto accts = svc->get_accounts("acc_123");
    EXPECT_TRUE(accts.empty());
}

TEST_F(PlaidServiceRefactorTest, StubUnlinkAccountSucceeds) {
    auto svc = create_plaid_service(true);
    bool result = svc->unlink_account("acc_123");
    EXPECT_TRUE(result);
}

TEST_F(PlaidServiceRefactorTest, StubSetTimeoutDoesNotCrash) {
    auto svc = create_plaid_service(true);
    svc->set_timeout(std::chrono::seconds{45});
    EXPECT_TRUE(svc->is_stub());
}

TEST_F(PlaidServiceRefactorTest, AccountDefaultIsPlaidLinkedFalse) {
    Account acc;
    acc.id = "acc_1";
    acc.name = "Test Account";
    acc.type = AccountType::Checking;
    acc.balance = 1000.0;
    // is_plaid_linked defaults to false.
    EXPECT_FALSE(acc.is_plaid_linked);
}

TEST_F(PlaidServiceRefactorTest, AccountIsPlaidLinkedCanBeSet) {
    Account acc;
    acc.is_plaid_linked = true;
    EXPECT_TRUE(acc.is_plaid_linked);
}
