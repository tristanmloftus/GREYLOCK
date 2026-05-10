// test_plaid_token_broker.cpp — Unit tests for server/plaid/PlaidTokenBroker.
//
// Uses an in-memory SQLite database with the M001 schema applied.
// All tests use a hard-coded 32-byte test master key (NOT from env var).
//
// Tests:
//   Store_RoundTrip             — store then withDecryptedToken returns same bytes.
//   Store_AadBinding            — token stored for account A cannot be read as B.
//   Clear_RemovesToken          — store, clear, withDecryptedToken → NoTokenTag path.
//   WithDecryptedToken_NoToken  — no store → NoTokenTag path invoked.
//   StoreDifferentAccounts      — tokens for A1/A2 are independent.
//   TwoCallableVariant          — two-callable overload works correctly.

#include <gtest/gtest.h>

#include "../server/plaid/PlaidTokenBroker.h"
#include "../server/db/Database.h"
#include "../server/db/Migrations.h"
#include "../src/services/crypto/Zeroize.h"

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sodium.h>

// ---------------------------------------------------------------------------
// Fixture: in-memory DB with M001 schema + a fixed 32-byte test master key.
// ---------------------------------------------------------------------------
class PlaidTokenBrokerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Ensure libsodium is initialized.
        if (sodium_init() < 0) {
            FAIL() << "sodium_init() failed";
        }

        // Create an in-memory database and apply M001.
        db_ = std::make_unique<Database>(":memory:");
        Migrations mig;
        mig.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
        mig.apply_pending(*db_);

        // Insert a test entity (required by accounts FK).
        db_->exec(
            "INSERT INTO entities (id, name, kind, created_at_unix) "
            "VALUES ('ent1', 'TestEntity', 'Individual', 0);");

        // Insert test accounts.
        db_->exec(
            "INSERT INTO accounts "
            "(id, entity_id, name, kind, balance_cents, created_at_unix) "
            "VALUES ('acc_a', 'ent1', 'AccountA', 'checking', 0, 0);");
        db_->exec(
            "INSERT INTO accounts "
            "(id, entity_id, name, kind, balance_cents, created_at_unix) "
            "VALUES ('acc_b', 'ent1', 'AccountB', 'checking', 0, 0);");

        // Build a fixed 32-byte test master key (all 0xAB for determinism).
        master_key_.fill(std::byte{0xAB});
    }

    void TearDown() override {
        db_.reset();
    }

    // Create a PlaidTokenBroker using the test master key.
    tf::plaid::PlaidTokenBroker make_broker() {
        return tf::plaid::PlaidTokenBroker(
            *db_,
            std::span<const std::byte, crypto_kdf_KEYBYTES>(master_key_.data(), master_key_.size()));
    }

    std::unique_ptr<Database> db_;
    std::array<std::byte, crypto_kdf_KEYBYTES> master_key_;
};

// ---------------------------------------------------------------------------
// Store_RoundTrip
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, Store_RoundTrip) {
    auto broker = make_broker();
    const std::string token = "access-sandbox-abc123";

    broker.store_token("acc_a", token);

    bool invoked = false;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view decrypted) {
            invoked = true;
            EXPECT_EQ(decrypted, token);
        },
        [&]() {
            FAIL() << "Expected token path, got NoToken path";
        });

    EXPECT_TRUE(invoked);
}

// ---------------------------------------------------------------------------
// Store_AadBinding
//
// Store a token for acc_a.  Then tamper the DB row so the account id in the
// row is now acc_b — decrypt must fail (AAD mismatch) rather than returning
// the wrong token.
//
// We simulate this by asking the broker to decrypt acc_a's blob as acc_b:
// we store for acc_a, then manually copy the encrypted_token to acc_b's row,
// and try to read via the broker (which uses acc_b as the AAD).
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, Store_AadBinding) {
    auto broker = make_broker();
    const std::string token = "access-prod-supersecret";

    // Store token under acc_a (AAD="acc_a").
    broker.store_token("acc_a", token);

    // Copy the blob to acc_b's row (simulating a cross-account move).
    db_->exec(
        "UPDATE accounts "
        "SET encrypted_token = (SELECT encrypted_token FROM accounts WHERE id='acc_a'), "
        "    is_plaid_linked = 1 "
        "WHERE id = 'acc_b';");

    // Trying to decrypt acc_b's row with AAD="acc_b" must fail.
    EXPECT_THROW(
        broker.withDecryptedToken(
            "acc_b",
            [](std::string_view) { /* should not be called */ },
            []() { /* no-token path: also should not reach here */ }),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// Clear_RemovesToken
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, Clear_RemovesToken) {
    auto broker = make_broker();
    broker.store_token("acc_a", "access-sandbox-token-to-clear");

    broker.clear_token("acc_a");

    bool no_token_called = false;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view) {
            FAIL() << "Expected NoToken path after clear; got token path";
        },
        [&]() {
            no_token_called = true;
        });

    EXPECT_TRUE(no_token_called);
}

// ---------------------------------------------------------------------------
// WithDecryptedToken_NoToken
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, WithDecryptedToken_NoToken) {
    // acc_a has no token stored.
    auto broker = make_broker();

    bool no_token_called = false;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view) {
            FAIL() << "Expected NoToken path; got token path";
        },
        [&]() {
            no_token_called = true;
        });

    EXPECT_TRUE(no_token_called);
}

// ---------------------------------------------------------------------------
// StoreDifferentAccounts
//
// Tokens for acc_a and acc_b are independent: each decrypts to its own value.
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, StoreDifferentAccounts) {
    auto broker = make_broker();
    const std::string token_a = "access-sandbox-account-a";
    const std::string token_b = "access-sandbox-account-b";

    broker.store_token("acc_a", token_a);
    broker.store_token("acc_b", token_b);

    bool a_ok = false;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view decrypted) {
            a_ok = true;
            EXPECT_EQ(decrypted, token_a);
        },
        [&]() { FAIL() << "acc_a: expected token path"; });

    bool b_ok = false;
    broker.withDecryptedToken(
        "acc_b",
        [&](std::string_view decrypted) {
            b_ok = true;
            EXPECT_EQ(decrypted, token_b);
        },
        [&]() { FAIL() << "acc_b: expected token path"; });

    EXPECT_TRUE(a_ok);
    EXPECT_TRUE(b_ok);
}

// ---------------------------------------------------------------------------
// TwoCallableVariant
//
// Verify that the two-callable withDecryptedToken works for both paths.
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, TwoCallableVariant) {
    auto broker = make_broker();

    // No-token path.
    int no_token_count = 0;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view) { FAIL() << "expected no-token path"; },
        [&]() { ++no_token_count; });
    EXPECT_EQ(no_token_count, 1);

    // Token path.
    broker.store_token("acc_a", "my-token");
    std::string captured;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view tok) { captured = std::string(tok); },
        [&]() { FAIL() << "expected token path"; });
    EXPECT_EQ(captured, "my-token");
}

// ---------------------------------------------------------------------------
// WithDecryptedToken_BufferZeroedAfterScope
//
// After withDecryptedToken returns, the ZeroizingBuffer used internally is
// out of scope and zeroed.  We verify this indirectly: if the callable
// captures a raw pointer to the token bytes, those bytes must not be readable
// after the scope exits (we check that the value returned inside is correct,
// but cannot directly inspect freed memory).
//
// What we CAN test: the value captured inside the scope matches the expected
// token, and no crash occurs.  The zeroing guarantee is proven by code review
// (ZeroizingBuffer's destructor calls sodium_memzero).
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, WithDecryptedToken_ValueCorrectInsideScope) {
    auto broker = make_broker();
    const std::string expected = "access-sandbox-zeroize-test";
    broker.store_token("acc_a", expected);

    std::string captured;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view tok) {
            // Inside scope: bytes are valid.
            captured = std::string(tok);
        },
        [&]() { FAIL() << "Expected token path"; });

    EXPECT_EQ(captured, expected);
    // Outside the scope, the ZeroizingBuffer is already destroyed and zeroed.
    // captured is a *copy* — this tests that the copy was made correctly.
}

// ---------------------------------------------------------------------------
// Store_OverwriteExistingToken
//
// Storing a second token for the same account replaces the first.
// ---------------------------------------------------------------------------
TEST_F(PlaidTokenBrokerTest, Store_OverwriteExistingToken) {
    auto broker = make_broker();
    broker.store_token("acc_a", "access-sandbox-first");
    broker.store_token("acc_a", "access-sandbox-second");

    std::string captured;
    broker.withDecryptedToken(
        "acc_a",
        [&](std::string_view tok) { captured = std::string(tok); },
        [&]() { FAIL() << "Expected token path"; });

    EXPECT_EQ(captured, "access-sandbox-second");
}
