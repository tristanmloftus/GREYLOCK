// snapshot_helper.h — TUI snapshot test infrastructure.
//
// Usage from a GoogleTest:
//
//   TEST(WidgetSnapshot, NetWorth) {
//       auto element = ftxui::NetWorthBreakdownRenderer(1234.56, 200.00,
//                                                      -500.00, 5000.00, 5934.56);
//       tf::snapshot::ExpectMatchesFixture(element, 80, 24, "net_worth");
//   }
//
// With the environment variable TF_UPDATE_SNAPSHOTS=1, ExpectMatchesFixture
// writes the captured output to tests/snapshot/fixtures/<name>.txt and the
// test passes.  Without the env var, the test reads the fixture and asserts
// the rendered output matches it exactly.
//
// Determinism notes (per Phase 5 brief):
//   - Line endings normalized to '\n'.
//   - UTF-8 throughout; widget code avoids platform-specific glyphs
//     (no unicode arrows/checkmarks in the promoted widgets).
//   - All snapshot inputs use frozen dates and whole-cent amounts —
//     no clock reads, no floating-point uncertainty.
//   - 80x24 fixed canvas; FTXUI border characters are platform-stable.
//
// Fixture path resolution:
//   1. If TF_FIXTURE_DIR is set, use $TF_FIXTURE_DIR/<name>.txt.
//   2. Else fall back to ./tests/snapshot/fixtures/<name>.txt (CWD-relative).
//      Snapshot tests are typically run via ctest from the build directory;
//      ctest sets WORKING_DIRECTORY to the source dir for this target
//      (see add_test in CMakeLists.txt).

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace tf::snapshot {

inline std::filesystem::path FixtureDir() {
    if (const char* env = std::getenv("TF_FIXTURE_DIR")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path("tests") / "snapshot" / "fixtures";
}

inline std::filesystem::path FixturePath(const std::string& name) {
    return FixtureDir() / (name + ".txt");
}

// Normalize line endings to '\n' (drop any trailing '\r' before each '\n').
inline std::string NormalizeNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r') continue;
        out.push_back(c);
    }
    return out;
}

inline std::string Render(const ftxui::Element& element, int width, int height) {
    // Make a fresh local copy of the element graph -- FTXUI's Render() may
    // mutate node layout state, so we operate on a copy via shared_ptr.
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height));
    ftxui::Element e = element;  // shared_ptr copy
    ftxui::Render(screen, e);
    return NormalizeNewlines(screen.ToString());
}

inline bool UpdateMode() {
    const char* env = std::getenv("TF_UPDATE_SNAPSHOTS");
    return env != nullptr && std::string(env) == "1";
}

inline void WriteFixture(const std::string& name, const std::string& contents) {
    const auto path = FixturePath(name);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "Failed to open fixture for write: " << path;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

inline std::string ReadFixture(const std::string& name) {
    const auto path = FixturePath(name);
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return NormalizeNewlines(ss.str());
}

// The main assertion helper.
//
// In capture mode (TF_UPDATE_SNAPSHOTS=1):
//   writes the rendered output to the fixture file and passes.
//
// In compare mode (default):
//   asserts the rendered output equals the fixture, with EXPECT_EQ so
//   GoogleTest prints a unified diff.
inline void ExpectMatchesFixture(
    const ftxui::Element& element,
    int width, int height,
    const std::string& fixture_name) {
    const std::string actual = Render(element, width, height);

    if (UpdateMode()) {
        WriteFixture(fixture_name, actual);
        SUCCEED() << "Captured fixture: " << FixturePath(fixture_name);
        return;
    }

    const std::string expected = ReadFixture(fixture_name);
    if (expected.empty()) {
        FAIL() << "Fixture not found: " << FixturePath(fixture_name)
               << "\nRun with TF_UPDATE_SNAPSHOTS=1 to create it.";
    }
    EXPECT_EQ(expected, actual)
        << "Snapshot diff for fixture '" << fixture_name << "'.\n"
        << "If the change is intentional, re-run with TF_UPDATE_SNAPSHOTS=1.";
}

}  // namespace tf::snapshot
