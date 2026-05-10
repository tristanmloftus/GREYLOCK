// tests/test_auth_totp.cpp — Phase 3 TOTP tests.
//
// Test cases (8):
//   1. GenerateSecretLength — 20 bytes
//   2. Base32Encoding — known vector
//   3. ProvisioningUriFormat — correct otpauth:// format
//   4. RFC6238ReferenceVector1 — SHA1/30s/6digit reference from RFC 4226 appendix
//   5. RFC6238ReferenceVector2 — second reference vector
//   6. GenerateAndRoundtrip — generate secret, compute code, verify accepts it
//   7. VerifyAcceptsSkewPlusMinus1 — code at t-30s and t+30s accepted
//   8. VerifyRejectsSkewPlusMinus2 — code at t±2 steps rejected by default window=1

#include <gtest/gtest.h>
#include <sodium.h>

#include "../server/auth/Totp.h"

#include <cstdint>
#include <string>
#include <vector>

class TotpEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        int rc = sodium_init();
        ASSERT_NE(rc, -1) << "sodium_init() failed";
    }
};

static ::testing::Environment* const gEnv =
    ::testing::AddGlobalTestEnvironment(new TotpEnvironment());

// ---------------------------------------------------------------------------
// RFC 4226 Appendix D reference vectors (HOTP, counter-based).
// RFC 6238 defines TOTP as HOTP with T = floor(unix_time / 30).
// These test vectors use counter values that correspond to specific T values:
//   T = counter, unix_seconds = counter * 30
//
// From RFC 4226 Appendix D, secret = "12345678901234567890" (20 ASCII bytes).
// Expected 6-digit codes for counters 0–9:
//   0: 755224
//   1: 287082
//   2: 359152
//   3: 969429
//   4: 338314
//   5: 254676
//   6: 287922
//   7: 162583
//   8: 399871
//   9: 520489
// ---------------------------------------------------------------------------

// Build the RFC 4226 test secret from ASCII "12345678901234567890".
static std::vector<std::byte> rfc4226_secret() {
    const char* s = "12345678901234567890";
    std::vector<std::byte> out;
    for (size_t i = 0; s[i]; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(s[i])));
    }
    return out;
}

// 1. GenerateSecretLength
TEST(Totp, GenerateSecretLength) {
    auto secret = tf::auth::generate_totp_secret();
    EXPECT_EQ(secret.size(), 20u);
}

// 2. Base32Encoding — RFC 4648 known vector.
// "foobar" in ASCII → bytes 0x66 0x6F 0x6F 0x62 0x61 0x72
// Expected base32: MZXW6YTBOI (per RFC 4648 test vector)
TEST(Totp, Base32Encoding) {
    const char raw[] = "foobar";
    std::vector<std::byte> bytes;
    for (size_t i = 0; raw[i]; ++i) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(raw[i])));
    }
    std::string b32 = tf::auth::totp_secret_to_base32(bytes);
    EXPECT_EQ(b32, "MZXW6YTBOI");
}

// 3. ProvisioningUriFormat
TEST(Totp, ProvisioningUriFormat) {
    auto secret = tf::auth::generate_totp_secret();
    std::string uri = tf::auth::make_totp_provisioning_uri(
        "alice@example.com", "TerminalFinance", secret);
    EXPECT_EQ(uri.substr(0, 15), "otpauth://totp/");
    EXPECT_NE(uri.find("issuer=TerminalFinance"), std::string::npos);
    EXPECT_NE(uri.find("algorithm=SHA1"), std::string::npos);
    EXPECT_NE(uri.find("digits=6"), std::string::npos);
    EXPECT_NE(uri.find("period=30"), std::string::npos);
    EXPECT_NE(uri.find("secret="), std::string::npos);
}

// 4. RFC 4226 reference vector: counter=0 → 755224.
TEST(Totp, RFC6238ReferenceVector_Counter0) {
    auto secret = rfc4226_secret();
    // unix_seconds such that floor(t/30) = 0: any t in [0, 29].
    int code = tf::auth::compute_totp_code(secret, 0);
    EXPECT_EQ(code, 755224);
}

// 5. RFC 4226 reference vector: counter=1 → 287082.
TEST(Totp, RFC6238ReferenceVector_Counter1) {
    auto secret = rfc4226_secret();
    // unix_seconds = 30 → floor(30/30) = 1.
    int code = tf::auth::compute_totp_code(secret, 30);
    EXPECT_EQ(code, 287082);
}

// Extra: counter=2 → 359152.
TEST(Totp, RFC6238ReferenceVector_Counter2) {
    auto secret = rfc4226_secret();
    int code = tf::auth::compute_totp_code(secret, 60);
    EXPECT_EQ(code, 359152);
}

// 6. GenerateAndRoundtrip — generate secret, compute code, verify accepts it.
TEST(Totp, GenerateAndRoundtrip) {
    auto secret = tf::auth::generate_totp_secret();
    int64_t t = 1000000LL; // arbitrary fixed time
    int code  = tf::auth::compute_totp_code(secret, t);
    EXPECT_TRUE(tf::auth::verify_totp_code(secret, t, code, 0));
}

// 7. VerifyAcceptsSkewPlusMinus1
TEST(Totp, VerifyAcceptsSkewPlusMinus1) {
    auto secret = tf::auth::generate_totp_secret();
    int64_t t = 1000000LL;

    // Code computed for t-30 (previous step) should verify with skew=1.
    int code_prev = tf::auth::compute_totp_code(secret, t - 30);
    EXPECT_TRUE(tf::auth::verify_totp_code(secret, t, code_prev, 1));

    // Code computed for t+30 (next step) should verify with skew=1.
    int code_next = tf::auth::compute_totp_code(secret, t + 30);
    EXPECT_TRUE(tf::auth::verify_totp_code(secret, t, code_next, 1));
}

// 8. VerifyRejectsSkewPlusMinus2 with default window=1.
TEST(Totp, VerifyRejectsSkewPlusMinus2) {
    auto secret = tf::auth::generate_totp_secret();
    int64_t t = 1000000LL;

    // Code for t±2 steps (±60s) should be rejected with default skew_window=1.
    // Note: there is a tiny probability that a different step's code happens to
    // equal the t±2 code; we use a long-enough fixed time to make that unlikely.
    int code_2prev = tf::auth::compute_totp_code(secret, t - 60);
    int code_curr  = tf::auth::compute_totp_code(secret, t);
    // If by coincidence code_2prev == code at t-1 or t+1 step, this would
    // produce a false pass; we just check it doesn't trivially equal the
    // current code (this is the main regression — the window is enforced).
    // The authoritative test: verify with window=1, code from t-2 steps.
    if (code_2prev != code_curr) {
        EXPECT_FALSE(tf::auth::verify_totp_code(secret, t, code_2prev, 1));
    }
    // And verify with window=2 accepts it.
    EXPECT_TRUE(tf::auth::verify_totp_code(secret, t, code_2prev, 2));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
