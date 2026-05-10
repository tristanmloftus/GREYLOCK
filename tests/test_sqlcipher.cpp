// tests/test_sqlcipher.cpp — Phase 4.E SQLCipher integration tests.
//
// Test cases (6):
//   1. Open_WithKey_AppliesPragma
//      — open :memory: with a key, PRAGMA cipher_version returns a version string.
//   2. Open_WithDifferentKey_FailsToReadExistingDb
//      — open file with key A, write row, close, reopen with key B, SELECT fails.
//   3. Open_WithSameKey_SucceedsToReadExistingDb
//      — write with key A, close, reopen with key A, SELECT returns the row.
//   4. Open_NoKey_OnEncryptedFile_Fails
//      — write with key A, close, reopen without key, SELECT fails.
//   5. Open_InvalidKey_NotHex_Throws
//      — pass a non-hex 64-char string, assert throws std::runtime_error.
//   6. Open_InvalidKey_WrongLength_Throws
//      — pass a 32-char hex string (16 bytes instead of 32), assert throws.
//
// All file-based tests use a temp path under /tmp/ and clean up in TearDown.

#include <gtest/gtest.h>

#include "../server/db/Database.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// Test key: 64 zeros = 32 zero bytes.  Well-known fixed key for tests.
// GUARDRAIL: test keys are hardcoded for reproducibility; production reads env.
static const std::string kKeyA(64, '0');
static const std::string kKeyB(64, 'a');  // different key

// Helper: generate a unique temp path.
static std::string tmp_db_path(const char* tag) {
    return std::string("/tmp/tf_sqlcipher_test_") + tag + ".db";
}

// Helper: remove a file if it exists.
static void rm_if_exists(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Test 1: Open :memory: with a key → cipher_version returns a version string.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_WithKey_AppliesPragma) {
    // Open in-memory database with the test key.
    Database db(":memory:", kKeyA);

    // PRAGMA cipher_version should return a non-empty string on SQLCipher.
    auto stmt = db.prepare("PRAGMA cipher_version;");
    int rc = stmt.step();
    ASSERT_EQ(rc, SQLITE_ROW) << "PRAGMA cipher_version returned no row";

    const unsigned char* ver = sqlite3_column_text(stmt.get(), 0);
    ASSERT_NE(ver, nullptr) << "cipher_version returned NULL";
    std::string ver_str(reinterpret_cast<const char*>(ver));
    EXPECT_FALSE(ver_str.empty()) << "cipher_version string is empty";

    // Should look like "4.x.y community" or similar — at minimum starts with "4.".
    EXPECT_EQ(ver_str.substr(0, 2), "4.") << "Unexpected cipher_version: " << ver_str;
}

// ---------------------------------------------------------------------------
// Test 2: Open file with key A, write row, close, reopen with key B → fail.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_WithDifferentKey_FailsToReadExistingDb) {
    const std::string path = tmp_db_path("diff_key");
    rm_if_exists(path);

    // Write with key A.
    {
        Database db(path, kKeyA);
        db.exec("CREATE TABLE t (v TEXT);");
        db.exec("INSERT INTO t VALUES ('hello');");
    }

    // Reopen with key B — should throw (master key mismatch).
    EXPECT_THROW(
        { Database db(path, kKeyB); },
        std::runtime_error
    ) << "Expected throw when opening encrypted DB with wrong key";

    rm_if_exists(path);
}

// ---------------------------------------------------------------------------
// Test 3: Write with key A, close, reopen with key A → SELECT returns row.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_WithSameKey_SucceedsToReadExistingDb) {
    const std::string path = tmp_db_path("same_key");
    rm_if_exists(path);

    // Write.
    {
        Database db(path, kKeyA);
        db.exec("CREATE TABLE t (v TEXT);");
        db.exec("INSERT INTO t VALUES ('world');");
    }

    // Reopen with same key — should succeed.
    Database db(path, kKeyA);
    auto stmt = db.prepare("SELECT v FROM t;");
    int rc = stmt.step();
    ASSERT_EQ(rc, SQLITE_ROW);
    const unsigned char* val = sqlite3_column_text(stmt.get(), 0);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(val)), "world");

    rm_if_exists(path);
}

// ---------------------------------------------------------------------------
// Test 4: Write with key A, close, reopen without key → first query throws.
//
// Note: SQLCipher's sqlite3_open() always returns SQLITE_OK regardless of
// whether the file is encrypted.  The error surfaces at the first prepared
// statement that actually reads page data.  Therefore we open the database
// without a key (which succeeds) and then assert that prepare() throws.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_NoKey_OnEncryptedFile_Fails) {
    const std::string path = tmp_db_path("no_key");
    rm_if_exists(path);

    // Write with key A.
    {
        Database db(path, kKeyA);
        db.exec("CREATE TABLE t (v TEXT);");
        db.exec("INSERT INTO t VALUES ('secret');");
    }

    // Reopen without key.  Construction succeeds (sqlite3_open always does).
    // The first real query — prepare() — must throw because the file is encrypted.
    {
        Database db(path, std::nullopt);
        EXPECT_THROW(
            { auto stmt = db.prepare("SELECT count(*) FROM sqlite_master;"); },
            std::runtime_error
        ) << "Expected throw when querying encrypted DB without key";
    }

    rm_if_exists(path);
}

// ---------------------------------------------------------------------------
// Test 5: Non-hex 64-char key → throws std::runtime_error.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_InvalidKey_NotHex_Throws) {
    // 64 chars but contains 'z' — invalid hex.
    std::string bad_key(64, 'z');
    EXPECT_THROW(
        { Database db(":memory:", bad_key); },
        std::runtime_error
    ) << "Expected throw for non-hex key";
}

// ---------------------------------------------------------------------------
// Test 6: 32-char hex (16 bytes instead of 32) → throws std::runtime_error.
// ---------------------------------------------------------------------------
TEST(SqlCipherTests, Open_InvalidKey_WrongLength_Throws) {
    std::string short_key(32, '0');  // 32 hex chars = 16 bytes — too short
    EXPECT_THROW(
        { Database db(":memory:", short_key); },
        std::runtime_error
    ) << "Expected throw for wrong-length key";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
