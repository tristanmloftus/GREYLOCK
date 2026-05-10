// tests/test_auth_enrollment_token.cpp — Phase 3 enrollment token tests.
//
// Test cases (5):
//   1. MintProduces64HexToken — raw_token is 64 lowercase hex chars
//   2. MintPersistConsumeRoundtrip — full happy path
//   3. ConsumeAfterExpiryReturnsNullopt — expired tokens rejected
//   4. ConsumeAlreadyConsumedReturnsNullopt — double-consume rejected
//   5. ConsumeWrongTokenReturnsNullopt — hash mismatch rejected

#include <gtest/gtest.h>
#include <sodium.h>

#include "../server/auth/EnrollmentToken.h"
#include "../server/db/Database.h"
#include "../server/db/Migrations.h"

#include <chrono>
#include <cstdint>
#include <string>

class EnrollmentTokenEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new EnrollmentTokenEnvironment());

// Phase 4.E: well-known test key (64 zeros = 32 zero bytes).
// Production reads TF_MASTER_KEY from env — never hardcoded there.
static const std::string kTestKey(64, '0');

// Helper: create an in-memory DB with M001 applied.
// Pass kTestKey so the SQLCipher key path is exercised in tests.
static Database make_test_db() {
    Database db(":memory:", kTestKey);
    Migrations m;
    m.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
    m.apply_pending(db);
    return db;
}

// 1. MintProduces64HexToken
TEST(EnrollmentToken, MintProduces64HexToken) {
    auto tok = tf::auth::mint_enrollment_token("test@example.com",
                                               std::chrono::seconds(3600));
    ASSERT_EQ(tok.raw_token.size(), 64u);
    for (char c : tok.raw_token) {
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(valid) << "Non-hex character: " << c;
    }
    EXPECT_EQ(tok.token_hash.size(), 32u); // BLAKE2b-256
}

// 2. MintPersistConsumeRoundtrip
TEST(EnrollmentToken, MintPersistConsumeRoundtrip) {
    auto db = make_test_db();

    const std::string email = "alice@example.com";
    auto tok = tf::auth::mint_enrollment_token(email, std::chrono::seconds(3600));
    tf::auth::persist_enrollment_token(db, tok, email);

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto rec = tf::auth::consume_enrollment_token(db, tok.raw_token, now);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->email, email);
    EXPECT_EQ(rec->token_hash, tok.token_hash);
    EXPECT_GT(rec->expires_at_unix, now);
}

// 3. ConsumeAfterExpiryReturnsNullopt
TEST(EnrollmentToken, ConsumeAfterExpiryReturnsNullopt) {
    auto db = make_test_db();

    const std::string email = "bob@example.com";
    // TTL = 1 second.
    auto tok = tf::auth::mint_enrollment_token(email, std::chrono::seconds(1));
    tf::auth::persist_enrollment_token(db, tok, email);

    // Consume at now + 10 (after the 1-second TTL).
    int64_t past = tok.expires_at_unix + 10;
    auto rec = tf::auth::consume_enrollment_token(db, tok.raw_token, past);
    EXPECT_FALSE(rec.has_value());
}

// 4. ConsumeAlreadyConsumedReturnsNullopt
TEST(EnrollmentToken, ConsumeAlreadyConsumedReturnsNullopt) {
    auto db = make_test_db();

    const std::string email = "carol@example.com";
    auto tok = tf::auth::mint_enrollment_token(email, std::chrono::seconds(3600));
    tf::auth::persist_enrollment_token(db, tok, email);

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // First consume succeeds.
    auto rec1 = tf::auth::consume_enrollment_token(db, tok.raw_token, now);
    ASSERT_TRUE(rec1.has_value());

    // Second consume fails.
    auto rec2 = tf::auth::consume_enrollment_token(db, tok.raw_token, now);
    EXPECT_FALSE(rec2.has_value());
}

// 5. ConsumeWrongTokenReturnsNullopt
TEST(EnrollmentToken, ConsumeWrongTokenReturnsNullopt) {
    auto db = make_test_db();

    const std::string email = "dave@example.com";
    auto tok = tf::auth::mint_enrollment_token(email, std::chrono::seconds(3600));
    tf::auth::persist_enrollment_token(db, tok, email);

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Try a completely different 64-char hex token.
    std::string wrong = std::string(64, '0');
    auto rec = tf::auth::consume_enrollment_token(db, wrong, now);
    EXPECT_FALSE(rec.has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
