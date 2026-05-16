#include <gtest/gtest.h>

#include <cstdlib>

#include "../src/utils/OpenBrowser.h"

// ---------------------------------------------------------------------------
// sanitize_url() — host policy
// ---------------------------------------------------------------------------

TEST(OpenBrowserTest, SanitizeUrl_AcceptsLocalhost) {
    EXPECT_TRUE(sanitize_url(
        "https://localhost:8443/link?account_id=test&link_token=abc"));
}

TEST(OpenBrowserTest, SanitizeUrl_Accepts127001) {
    EXPECT_TRUE(sanitize_url("https://127.0.0.1:8443/link?account_id=x"));
}

TEST(OpenBrowserTest, SanitizeUrl_AcceptsBackendUrlHost) {
    setenv("TF_BACKEND_URL",
           "https://skynet-debian.tail-abc.ts.net:8443", 1);
    EXPECT_TRUE(sanitize_url(
        "https://skynet-debian.tail-abc.ts.net:8443/link?account_id=x"));
    unsetenv("TF_BACKEND_URL");
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsCdnPlaidComStillExcluded) {
    // cdn.plaid.com is loaded inside the HTML Link page on our server,
    // not opened directly by the TUI. Sanitizer must reject it.
    EXPECT_FALSE(sanitize_url(
        "https://cdn.plaid.com/link/v2/stable/link-initialize.html"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsHttp) {
    EXPECT_FALSE(sanitize_url("http://localhost:8443/link"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsUnknownHost) {
    EXPECT_FALSE(sanitize_url("https://evil.com/hack"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsEmpty) {
    EXPECT_FALSE(sanitize_url(""));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsHostnameSuffixAttack) {
    // localhost.evil.com should NOT be treated as localhost.
    EXPECT_FALSE(sanitize_url("https://localhost.evil.com:8443/link"));
}

// ---------------------------------------------------------------------------
// sanitize_url() — control chars and shell metacharacters
//
// open_browser() uses argv-based exec, so these can't actually trigger
// shell injection via the current path. The tests document the threat
// model and protect against any future regression to a shell-based opener.
// ---------------------------------------------------------------------------

TEST(OpenBrowserTest, SanitizeUrl_RejectsControlChars) {
    EXPECT_FALSE(sanitize_url("https://localhost/link\n<script>"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsCommandSubstitution) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=$(rm -rf /)"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsBacktick) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=`whoami`"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsSemicolon) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=x;rm"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsPipe) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=x|nc"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsBackslash) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=x\\y"));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsDoubleQuote) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=\";touch /tmp/x;\""));
}

TEST(OpenBrowserTest, SanitizeUrl_RejectsSingleQuote) {
    EXPECT_FALSE(sanitize_url(
        "https://localhost:8443/link?id=x'evil"));
}

// ---------------------------------------------------------------------------
// sanitize_url() — legitimate URL syntax that must still pass
// ---------------------------------------------------------------------------

TEST(OpenBrowserTest, SanitizeUrl_AcceptsAmpersandInQuery) {
    // & is the URL query-string separator; must remain legal.
    EXPECT_TRUE(sanitize_url(
        "https://localhost:8443/link?account_id=abc&link_token=xyz"));
}

TEST(OpenBrowserTest, SanitizeUrl_AcceptsFragment) {
    EXPECT_TRUE(sanitize_url("https://localhost:8443/link#step-2"));
}

TEST(OpenBrowserTest, SanitizeUrl_AcceptsUrlEncodedQuery) {
    EXPECT_TRUE(sanitize_url(
        "https://localhost:8443/link?account_id=a%20b&link_token=c%2Bd"));
}
