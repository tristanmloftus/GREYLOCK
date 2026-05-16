#include <gtest/gtest.h>

#include "../src/utils/OpenBrowser.h"

// ---------------------------------------------------------------------------
// SanitizeUrl tests
// ---------------------------------------------------------------------------

TEST(OpenBrowserTest, SanitizeUrl_AcceptsPlaidLink) {
    EXPECT_TRUE(sanitize_url("https://cdn.plaid.com/link/v2/stable/link-initialize.html?token=link-sandbox-abc123"));
}

TEST(OpenBrowserTest, SanitizeUrl_AcceptsLocalhost) {
    EXPECT_TRUE(sanitize_url("https://localhost:8443/link?account_id=test&link_token=abc"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsHttp) {
    EXPECT_FALSE(sanitize_url("http://evil.com/hack"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsUnknownHost) {
    EXPECT_FALSE(sanitize_url("https://evil.com/hack"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsEmpty) {
    EXPECT_FALSE(sanitize_url(""));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsControlChars) {
    EXPECT_FALSE(sanitize_url("https://cdn.plaid.com/link\n<script>"));
}
