// tests/test_auth_session.cpp — Phase 3 session lifecycle tests.
//
// Test cases (5):
//   1. MintAndValidateSucceeds — fresh session validates immediately
//   2. ValidateAfterIdleExpiryReturnsNullopt — idle window expired
//   3. ValidateAfterAbsoluteExpiryReturnsNullopt — absolute timeout expired
//   4. RevokeWorks — revoked session rejected by validate
//   5. ValidateWrongTokenReturnsNullopt — hash mismatch

#include <gtest/gtest.h>
#include <sodium.h>

#include "../server/auth/Session.h"
#include "../server/db/Database.h"
#include "../server/db/Migrations.h"

#include <cstdint>
#include <string>

class SessionEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new SessionEnvironment());

// Helper: create an in-memory DB with a dummy users row so FK constraint passes.
static Database make_test_db() {
    Database db(":memory:");
    Migrations m;
    m.register_migration({1, "M001_initial_schema", M001_initial_schema_up});
    m.apply_pending(db);
    // Insert a test user (passphrase_hash = empty blob to satisfy NOT NULL).
    db.exec(
        "INSERT INTO users (id, email, created_at_unix, passphrase_hash) "
        "VALUES ('test-user-id', 'test@example.com', 0, X'');"
    );
    return db;
}

static const char* kUserId = "test-user-id";

// 1. MintAndValidateSucceeds
TEST(Session, MintAndValidateSucceeds) {
    auto db = make_test_db();
    int64_t t = 1000000LL;

    auto s = tf::auth::mint_session(db, kUserId, t);
    EXPECT_FALSE(s.raw_token.empty());

    auto uid = tf::auth::validate_and_touch_session(db, s.raw_token, t + 1);
    ASSERT_TRUE(uid.has_value());
    EXPECT_EQ(*uid, kUserId);
}

// 2. ValidateAfterIdleExpiryReturnsNullopt
TEST(Session, ValidateAfterIdleExpiryReturnsNullopt) {
    auto db = make_test_db();
    int64_t t = 1000000LL;

    auto s = tf::auth::mint_session(db, kUserId, t);

    // Advance time past idle window (30 min + 1 s).
    int64_t idle_expired = t + tf::auth::kSessionIdleTimeoutSeconds + 1;
    auto uid = tf::auth::validate_and_touch_session(db, s.raw_token, idle_expired);
    EXPECT_FALSE(uid.has_value());
}

// 3. ValidateAfterAbsoluteExpiryReturnsNullopt
TEST(Session, ValidateAfterAbsoluteExpiryReturnsNullopt) {
    auto db = make_test_db();
    int64_t t = 1000000LL;

    auto s = tf::auth::mint_session(db, kUserId, t);

    // Advance time past absolute window (8h + 1s).
    int64_t abs_expired = t + tf::auth::kSessionAbsoluteTimeoutSeconds + 1;
    auto uid = tf::auth::validate_and_touch_session(db, s.raw_token, abs_expired);
    EXPECT_FALSE(uid.has_value());
}

// 4. RevokeWorks
TEST(Session, RevokeWorks) {
    auto db = make_test_db();
    int64_t t = 1000000LL;

    auto s = tf::auth::mint_session(db, kUserId, t);

    // Revoke the session.
    bool ok = tf::auth::revoke_session(db, s.raw_token);
    EXPECT_TRUE(ok);

    // Validate should now fail.
    auto uid = tf::auth::validate_and_touch_session(db, s.raw_token, t + 1);
    EXPECT_FALSE(uid.has_value());
}

// 5. ValidateWrongTokenReturnsNullopt
TEST(Session, ValidateWrongTokenReturnsNullopt) {
    auto db = make_test_db();
    int64_t t = 1000000LL;

    auto s = tf::auth::mint_session(db, kUserId, t);
    (void)s;

    // Use a fabricated token that will produce a different hash.
    // base64url of 32 zero bytes:
    std::string wrong_token = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    auto uid = tf::auth::validate_and_touch_session(db, wrong_token, t + 1);
    EXPECT_FALSE(uid.has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
