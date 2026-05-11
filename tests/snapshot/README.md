# TUI Snapshot Harness

Regression coverage for the FTXUI widgets in `src/views/widgets/`.

Each test renders one widget at a fixed `80x24` canvas with frozen,
whole-cent inputs and compares the byte output to a checked-in fixture
under `fixtures/`.

## Running

From the worktree root:

```bash
# Build (one-time)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target WidgetSnapshotTests

# Run
ctest --test-dir build -R WidgetSnapshot --output-on-failure
```

## Capturing or refreshing fixtures

When a widget renderer changes intentionally, regenerate the fixtures:

```bash
TF_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R WidgetSnapshot --output-on-failure
# inspect git diff tests/snapshot/fixtures/
git add tests/snapshot/fixtures/
```

The tests pass automatically in capture mode (they write the fixture and
return success). Always inspect the diff before committing — if anything
unexpected changed, fix the widget instead of accepting the new fixture.

## Determinism rules

These are required for the suite to be regression-stable on both macOS
and Linux/Windows CI:

- All inputs are constants (frozen dates `2026-04-01`, whole-cent
  amounts like `1234.56`). **No clock reads or `std::random` in test
  setup.**
- Line endings are normalized to `\n` by `snapshot_helper.h`. Don't add
  fixtures with CRLF.
- Widget code uses ASCII for status icons / arrows (`[+]`, `[x]`, `^`,
  `v`, `-`). Multi-byte UTF-8 glyphs render with different widths on
  different terminals, which would flake the comparison.
- Canvas is `80x24` (`W`, `H` constants in `test_widgets_snapshot.cpp`).
  Don't change this without regenerating every fixture.

## Adding a new snapshot

1. Add a widget renderer (`Element FooRenderer(...)`).
2. Add a `TEST(WidgetSnapshot, Foo)` in `tests/test_widgets_snapshot.cpp`.
3. Run with `TF_UPDATE_SNAPSHOTS=1` to capture the initial fixture.
4. Inspect the captured fixture for sanity.
5. Commit both the test and the fixture.
