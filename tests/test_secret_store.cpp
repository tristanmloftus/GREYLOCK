// test_secret_store.cpp — Phase 1.A GoogleTest binary for ISecretStore implementations.
//
// SANDBOXING STRATEGY
// -------------------
// All test keys are prefixed with "tf-test-" to guarantee they can never collide
// with production keys stored under bare names like "plaid_access_token".
//
// On macOS the KeychainSecretStore stores items under kSecAttrService =
// "com.terminalfinance.secrets". Test items are stored under the same service
// but with account names prefixed "tf-test-", which is a distinct namespace from
// any production key the app stores (all production keys are bare names like
// "plaid_access_token_<id>"). TearDown() removes every registered key, so test
// items are cleaned up even on assertion failure.
//
// On Windows the DpapiSecretStore uses HKCU\Software\Greylock.Tokens. Tests
// use the "tf-test-" prefix on all value names; no registry subkey override is needed
// because the prefix is a sufficient safety belt.
//
// CLEANUP
// -------
// The fixture's TearDown() removes every key it registered via track_key(). All test
// cases call track_key() before any put() so cleanup runs even when the test assertion
// fails mid-way.
//
// BUILD
// -----
// macOS: linked against -framework Security -framework CoreFoundation.
// Windows: linked against crypt32 advapi32.
//
// CI / Tristan commands (Windows):
//   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
//   cmake --build build --config Release --target SecretStoreTests
//   ctest --test-dir build -C Release -R SecretStoreTests --output-on-failure

#include <gtest/gtest.h>
#include "../src/services/ISecretStore.h"

#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

// ---- Platform dispatch ----
#ifdef _WIN32
#  include "../src/services/secret_store/DpapiSecretStore.h"
#elif defined(__APPLE__)
#  include "../src/services/secret_store/KeychainSecretStore.h"
#endif

// ---- Helpers ----

// Convert a std::string_view to a span<const std::byte>.
static std::span<const std::byte> to_span(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

static std::span<const std::byte> to_span(const std::vector<std::byte>& v) {
    return {v.data(), v.size()};
}

// Convert an optional<vector<byte>> result back to a string for comparison.
static std::string bytes_to_string(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// ============================================================
// Fixture
// ============================================================

class SecretStoreTest : public ::testing::Test {
protected:
    std::unique_ptr<ISecretStore> store_;
    std::vector<std::string> registered_keys_;

    void SetUp() override {
#ifdef _WIN32
        store_ = std::make_unique<DpapiSecretStore>();
#elif defined(__APPLE__)
        store_ = std::make_unique<KeychainSecretStore>();
#else
        GTEST_SKIP() << "No ISecretStore implementation for this platform.";
#endif
    }

    void TearDown() override {
        if (!store_) return;
        for (const auto& k : registered_keys_) {
            store_->remove(k);
        }
        registered_keys_.clear();
    }

    // Call this before any put() so TearDown always cleans up, even on failure.
    void track_key(const std::string& key) {
        registered_keys_.push_back(key);
    }

    // Convenience: build a "tf-test-<suffix>" key and track it.
    std::string make_key(const std::string& suffix) {
        std::string k = "tf-test-" + suffix;
        track_key(k);
        return k;
    }
};

// ============================================================
// Test 1 — PutGetRoundTripAscii
// ============================================================
TEST_F(SecretStoreTest, PutGetRoundTripAscii) {
    std::string key = make_key("ascii");
    // 64-byte printable ASCII payload.
    std::string payload(64, 'A');
    for (int i = 0; i < 64; ++i) payload[static_cast<size_t>(i)] = static_cast<char>('A' + (i % 26));

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(bytes_to_string(*result), payload);
}

// ============================================================
// Test 2 — PutGetRoundTrip10K
// ============================================================
TEST_F(SecretStoreTest, PutGetRoundTrip10K) {
    std::string key = make_key("10k");
    // 10,240-byte payload (plan exit criterion).
    std::string payload(10'240, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(i % 256));
    }

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), payload.size());
    EXPECT_EQ(0, std::memcmp(result->data(), payload.data(), payload.size()));
}

// ============================================================
// Test 3 — PutGetRoundTripUtf8
// ============================================================
TEST_F(SecretStoreTest, PutGetRoundTripUtf8) {
    std::string key = make_key("utf8");
    // UTF-8 string with multibyte characters.
    // "héllo🔐" — 'é' is U+00E9 (2 bytes), '🔐' is U+1F510 (4 bytes).
    // We use a plain string literal (not u8"") so it stays std::string in C++20.
    // The source file is UTF-8 so the byte values are correct.
    std::string payload = "héllo🔐 wörld — café naïve résumé";

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), payload.size());
    EXPECT_EQ(0, std::memcmp(result->data(), payload.data(), payload.size()));
}

// ============================================================
// Test 4 — PutGetRoundTripBinary
// ============================================================
TEST_F(SecretStoreTest, PutGetRoundTripBinary) {
    std::string key = make_key("binary");
    // Full byte range 0x00..0xFF — catches null-terminator bugs in either backend.
    std::vector<std::byte> payload(256);
    for (size_t i = 0; i < 256; ++i) payload[i] = static_cast<std::byte>(i);

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), payload.size());
    EXPECT_EQ(0, std::memcmp(result->data(), payload.data(), payload.size()));
}

// ============================================================
// Test 5 — GetMissingReturnsNullopt
// ============================================================
TEST_F(SecretStoreTest, GetMissingReturnsNullopt) {
    // This key is tracked so TearDown is a no-op (remove on missing = success).
    std::string key = make_key("missing-get");
    // Do NOT put anything.
    auto result = store_->get(key);
    EXPECT_FALSE(result.has_value()) << "get() on a never-put key must return nullopt";
}

// ============================================================
// Test 6 — RemoveMissingReturnsTrue
// ============================================================
TEST_F(SecretStoreTest, RemoveMissingReturnsTrue) {
    std::string key = make_key("missing-remove");
    // Do NOT put anything. Remove must still succeed (idempotency contract).
    EXPECT_TRUE(store_->remove(key));
}

// ============================================================
// Test 7 — RemoveExistingThenGetReturnsNullopt
// ============================================================
TEST_F(SecretStoreTest, RemoveExistingThenGetReturnsNullopt) {
    std::string key = make_key("remove-then-get");
    std::string payload = "delete-me";

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    ASSERT_TRUE(store_->remove(key));

    auto result = store_->get(key);
    EXPECT_FALSE(result.has_value()) << "get() after remove() must return nullopt";
}

// ============================================================
// Test 8 — PutOverwrites
// ============================================================
TEST_F(SecretStoreTest, PutOverwrites) {
    std::string key = make_key("overwrite");
    std::string first_value = "first-value-abc";
    std::string second_value = "second-value-XYZ";

    ASSERT_TRUE(store_->put(key, to_span(first_value)));
    ASSERT_TRUE(store_->put(key, to_span(second_value)));

    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(bytes_to_string(*result), second_value)
        << "Second put() must overwrite the first value, not append or error";
}

// ============================================================
// Test 9 — Stress64K
// ============================================================
TEST_F(SecretStoreTest, Stress64K) {
    std::string key = make_key("stress-64k");
    // 64 KiB payload — catches size-limit bugs in either backend.
    constexpr size_t kSize = 64u * 1024u;
    std::vector<std::byte> payload(kSize);
    for (size_t i = 0; i < kSize; ++i) payload[i] = static_cast<std::byte>(i % 256);

    ASSERT_TRUE(store_->put(key, to_span(payload)));
    auto result = store_->get(key);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), kSize);
    EXPECT_EQ(0, std::memcmp(result->data(), payload.data(), kSize));
}

// ============================================================
// Test 10 — MultipleKeysIndependent
// ============================================================
TEST_F(SecretStoreTest, MultipleKeysIndependent) {
    std::string key_a = make_key("multi-a");
    std::string key_b = make_key("multi-b");
    std::string key_c = make_key("multi-c");

    std::string val_a = "value-for-A";
    std::string val_b = "value-for-B";
    std::string val_c = "value-for-C";

    ASSERT_TRUE(store_->put(key_a, to_span(val_a)));
    ASSERT_TRUE(store_->put(key_b, to_span(val_b)));
    ASSERT_TRUE(store_->put(key_c, to_span(val_c)));

    // Verify all three are present.
    {
        auto ra = store_->get(key_a);
        ASSERT_TRUE(ra.has_value());
        EXPECT_EQ(bytes_to_string(*ra), val_a);

        auto rb = store_->get(key_b);
        ASSERT_TRUE(rb.has_value());
        EXPECT_EQ(bytes_to_string(*rb), val_b);

        auto rc = store_->get(key_c);
        ASSERT_TRUE(rc.has_value());
        EXPECT_EQ(bytes_to_string(*rc), val_c);
    }

    // Remove key_b; the other two must be unaffected.
    ASSERT_TRUE(store_->remove(key_b));

    {
        auto ra = store_->get(key_a);
        ASSERT_TRUE(ra.has_value()) << "key_a must survive removal of key_b";
        EXPECT_EQ(bytes_to_string(*ra), val_a);

        auto rb = store_->get(key_b);
        EXPECT_FALSE(rb.has_value()) << "key_b must be gone after remove()";

        auto rc = store_->get(key_c);
        ASSERT_TRUE(rc.has_value()) << "key_c must survive removal of key_b";
        EXPECT_EQ(bytes_to_string(*rc), val_c);
    }

    // Remove remaining keys. TearDown will also attempt removal (idempotent).
    EXPECT_TRUE(store_->remove(key_a));
    EXPECT_TRUE(store_->remove(key_c));
}

// ============================================================
// Entry point
// ============================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
