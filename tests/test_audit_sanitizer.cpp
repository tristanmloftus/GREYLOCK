// tests/test_audit_sanitizer.cpp — Sanitizer unit tests.
//
// Test cases (8):
//   1. AllowedKeys_PassThrough — safe allowed keys pass.
//   2. PasswordField_Rejected — "password" key rejected.
//   3. SecretField_Rejected — "secret" key rejected.
//   4. TokenField_Rejected — "token" key rejected.
//   5. TokenId_Allowed — "tokenId" carve-out allowed.
//   6. Base64ShapeString_Rejected — 33-char base64-shaped string rejected.
//   7. HexShapeString_Rejected — 64-char hex string rejected.
//   8. OversizedPayload_Rejected — 65 KiB payload rejected.
//   9. DeepNested_Rejected — 9-deep nested object rejected.
//  10. EmptyPayload_Allowed — empty {} passes.

#include "server/audit/Sanitizer.h"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <string>

using json = nlohmann::json;
using tf::audit::SanitizerStatus;
using tf::audit::sanitize;

// ---------------------------------------------------------------------------
// 1. Allowed keys pass through.
// ---------------------------------------------------------------------------
TEST(Sanitizer, AllowedKeys_PassThrough) {
    json details = {
        {"userId",        "abc123"},
        {"sessionId",     "sess456"},
        {"action",        "login"},
        {"outcome",       "success"},
        {"kind",          "user"},
        {"httpStatus",    200},
        {"count",         5}
    };
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Accepted)
        << "reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 2. "password" key rejected.
// ---------------------------------------------------------------------------
TEST(Sanitizer, PasswordField_Rejected) {
    json details = {{"password", "hunter2"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected);
}

// ---------------------------------------------------------------------------
// 3. "secret" key rejected.
// ---------------------------------------------------------------------------
TEST(Sanitizer, SecretField_Rejected) {
    json details = {{"secret", "my-secret-value"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected);
}

// ---------------------------------------------------------------------------
// 4. "token" key rejected.
// ---------------------------------------------------------------------------
TEST(Sanitizer, TokenField_Rejected) {
    json details = {{"token", "some-token-value"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected);
}

// ---------------------------------------------------------------------------
// 5. "tokenId" is explicitly in the allowlist (carve-out from "token").
// ---------------------------------------------------------------------------
TEST(Sanitizer, TokenId_Allowed) {
    json details = {{"tokenId", "tok_row_id_123"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Accepted)
        << "tokenId should be allowed (carve-out from 'token'). reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 6. 33-char base64-shaped string rejected (token-shape value).
// ---------------------------------------------------------------------------
TEST(Sanitizer, Base64ShapeString_Rejected) {
    // 33 chars of base64url chars — should match token-shape.
    std::string b64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; // 34 chars, all 'A'
    // Trim to 33 chars and confirm it still triggers.
    b64 = b64.substr(0, 33);
    json details = {{"kind", b64}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "33-char base64-shaped value should be rejected. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 7. 64-char hex string rejected (token-shape value).
// ---------------------------------------------------------------------------
TEST(Sanitizer, HexShapeString_Rejected) {
    std::string hex(64, 'a'); // "aaaaaaa...a" — 64 hex chars
    json details = {{"kind", hex}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "64-char hex string should be rejected. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 8. 65 KiB payload rejected.
// ---------------------------------------------------------------------------
TEST(Sanitizer, OversizedPayload_Rejected) {
    // Build a payload with a single "reason" key whose value pushes the
    // serialized JSON over 64 KiB.
    std::string big_value(66 * 1024, 'x'); // 66 KiB of 'x'
    json details = {{"reason", big_value}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "65+ KiB payload should be rejected. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 9. 9-deep nested object rejected (depth cap = 8).
// ---------------------------------------------------------------------------
TEST(Sanitizer, DeepNested_Rejected) {
    // Build a structure exceeding the depth cap.
    // The sanitizer walks the root at depth=0 and rejects when depth > 8.
    // Each wrapper level adds 1. Root is level 0, so 9 wrappers puts the
    // innermost object at depth=9, which exceeds MAX_DEPTH=8 and is rejected.
    json inner = {{"reason", "deep"}};
    for (int i = 0; i < 9; ++i) {
        inner = {{"kind", inner}};
    }
    // inner is now: {kind: {kind: {kind: ... (9 wrappers) ... {reason: deep}}}}
    // The 9th wrapper's value is visited at depth=9 > MAX_DEPTH=8 → rejected.
    auto result = sanitize(inner);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "9-deep nested object should be rejected. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// 10. Empty payload {} passes.
// ---------------------------------------------------------------------------
TEST(Sanitizer, EmptyPayload_Allowed) {
    json details = json::object();
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Accepted)
        << "Empty payload should be accepted. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// Additional edge-case tests.
// ---------------------------------------------------------------------------

// Case-insensitive deny: "Password" (capital P) must also be rejected.
TEST(Sanitizer, CaseInsensitiveKeyDeny_Rejected) {
    json details = {{"Password", "hunter2"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "Case-insensitive 'Password' should be rejected";
}

// "key" substring: a key named "apiKey" should be rejected.
TEST(Sanitizer, ApiKeyField_Rejected) {
    json details = {{"apiKey", "some-value"}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Rejected)
        << "'apiKey' contains 'key' and should be rejected";
}

// Short strings (< 32 chars) of base64 shape should be allowed.
TEST(Sanitizer, ShortBase64String_Allowed) {
    std::string short_b64(16, 'A'); // 16 chars — below the 32-char floor
    json details = {{"reason", short_b64}};
    auto result = sanitize(details);
    EXPECT_EQ(result.status, SanitizerStatus::Accepted)
        << "Short base64-like string should be allowed. reason: " << result.reason;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
