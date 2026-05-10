// tests/test_auth_passphrase.cpp — Phase 3 passphrase hashing tests.
//
// Test cases (5):
//   1. HashAndVerifySamePassphrase — returns true
//   2. HashAndVerifyWrongPassphrase — returns false
//   3. TwoHashesDiffer — random salt ensures different output bytes
//   4. HashIsSlow — >50ms wall time (Argon2id cost property)
//   5. StoredHashSize — exactly crypto_pwhash_STRBYTES bytes

#include <gtest/gtest.h>
#include <sodium.h>

#include "../server/auth/PassphraseHash.h"

#include <chrono>
#include <string>
#include <vector>

class PassphraseHashEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new PassphraseHashEnvironment());

// 1. Hash + verify same passphrase → true.
TEST(PassphraseHash, HashAndVerifySamePassphrase) {
    auto hash = tf::auth::hash_passphrase("correct-horse-battery-staple");
    EXPECT_TRUE(tf::auth::verify_passphrase("correct-horse-battery-staple", hash));
}

// 2. Hash + verify wrong passphrase → false.
TEST(PassphraseHash, HashAndVerifyWrongPassphrase) {
    auto hash = tf::auth::hash_passphrase("correct-horse-battery-staple");
    EXPECT_FALSE(tf::auth::verify_passphrase("wrong-passphrase", hash));
}

// 3. Two hashes of the same passphrase differ (random salt).
TEST(PassphraseHash, TwoHashesDiffer) {
    auto hash1 = tf::auth::hash_passphrase("same-passphrase");
    auto hash2 = tf::auth::hash_passphrase("same-passphrase");
    // They must differ because of the random salt embedded in the hash string.
    EXPECT_NE(hash1, hash2);
    // But both must verify correctly.
    EXPECT_TRUE(tf::auth::verify_passphrase("same-passphrase", hash1));
    EXPECT_TRUE(tf::auth::verify_passphrase("same-passphrase", hash2));
}

// 4. Hashing is slow-by-design (Argon2id MODERATE ≥ 50ms).
TEST(PassphraseHash, HashIsSlow) {
    auto t0 = std::chrono::steady_clock::now();
    auto hash = tf::auth::hash_passphrase("timing-test-passphrase");
    auto t1 = std::chrono::steady_clock::now();
    (void)hash;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_GE(elapsed_ms, 50)
        << "Argon2id hash took " << elapsed_ms
        << " ms — should be ≥50ms for MODERATE parameters";
}

// 5. Stored hash is exactly crypto_pwhash_STRBYTES bytes.
TEST(PassphraseHash, StoredHashSize) {
    auto hash = tf::auth::hash_passphrase("size-check");
    EXPECT_EQ(hash.size(), static_cast<size_t>(crypto_pwhash_STRBYTES));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
