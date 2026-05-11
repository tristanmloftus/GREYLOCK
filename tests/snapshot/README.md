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

## Determinism contract (Phase 6)

The snapshot harness is a regression gate, not a portability test. It
relies on a small set of invariants — break any one and snapshots will
flake across machines. Tag this section when reviewing changes that
touch widget code, fixture files, or `snapshot_helper.h`.

| Invariant                  | Where enforced                                       | Why it matters                                                                                                |
|----------------------------|------------------------------------------------------|---------------------------------------------------------------------------------------------------------------|
| **80×24 fixed canvas**     | `tests/test_widgets_snapshot.cpp` (`W`, `H` consts)  | Removes any dependency on `tput cols` / `$COLUMNS` / `ioctl(TIOCGWINSZ)`. The harness builds a `Screen` with `Dimension::Fixed`, so terminal size is irrelevant. |
| **`\n` line endings**      | `snapshot_helper.h::NormalizeNewlines`               | Strips all `\r` bytes before compare *and* before fixture write. CRLF on Windows runners, lone CR on Mac classic — both become LF. Fixtures must be checked in with LF (`file fixtures/*.txt` should not say `CRLF line terminators`). |
| **No locale-sensitive output** | Widget renderers + frozen fixture inputs           | Widget code uses `printf("%.2f")` and integer dates only — no `setlocale`, no thousands separators, no `std::format` locale facets. A fixture flaked by `LC_NUMERIC=fr_FR` would surface as a comma instead of a period. None do today; keep it that way. |
| **ASCII-only widget glyphs** | `src/views/widgets/*.cpp`                          | Status icons, arrows, and decorative chars are 1-byte ASCII (`[+] [x] ^ v -`). Multi-byte UTF-8 changes column width in some terminals and would shift the FTXUI layout in unpredictable ways. |
| **FTXUI box-drawing UTF-8 is OK** | `_deps/ftxui-src/` (pinned to v5.0.0 by FetchContent) | The `╭─╮│├┤╰╯` characters in fixtures come from FTXUI's `border()` element, not our code. Because FTXUI is bundled from a pinned source tag, the exact UTF-8 byte sequence is identical on every host — Linux self-hosted, macOS dev, Windows MSVC. **Do not "convert to ASCII"**; that would diverge from the library's own output and require maintaining a custom border style. |
| **Frozen test inputs**     | `tests/test_widgets_snapshot.cpp`                   | No `time()`, no `std::random_device`, no `$HOME`, no hostname. Each `TEST()` body literally constructs the inputs the renderer expects. |
| **`Screen::ToString()` is deterministic** | FTXUI `screen.cpp::ToString()`             | Walks the screen buffer in row-major order and emits ANSI SGR sequences inline. There is no clock, no entropy, no platform-conditional code path in this function. |

### How to detect drift

If a snapshot test starts failing on one OS but not another, in order:

1. Run `file tests/snapshot/fixtures/*.txt` — confirm every fixture is
   `ASCII text` or `UTF-8 Unicode text` with no `CRLF` suffix.
2. Run `cmp <(LC_ALL=C cat fixture_a.txt) <(LC_ALL=C cat fixture_b.txt)` on
   the rendered output captured from each OS. The diff should be empty.
3. Inspect the widget `.cpp` for any `\u` escape, `wchar_t`, `std::format`
   with locale, or `time(nullptr)` introduced since the last green run.
4. Confirm FTXUI is still pinned to `v5.0.0` in the root `CMakeLists.txt`.
   A point upgrade can change box-drawing or text-element ANSI output
   and would require fresh fixtures.
5. If steps 1–4 are clean and the diff is real, the renderer changed
   intentionally — regenerate fixtures with `TF_UPDATE_SNAPSHOTS=1` and
   commit the diff with the renderer change.

The suite empirically passes on macOS (AppleClang) and Linux
(GCC, self-hosted Debian 13) on identical bytes. Windows MSVC is
expected to also pass on the same bytes because FTXUI v5.0.0 is the
sole source of non-ASCII output and is bundled deterministically; it
has not been wired into CI as of Phase 6.

## Adding a new snapshot

1. Add a widget renderer (`Element FooRenderer(...)`).
2. Add a `TEST(WidgetSnapshot, Foo)` in `tests/test_widgets_snapshot.cpp`.
3. Run with `TF_UPDATE_SNAPSHOTS=1` to capture the initial fixture.
4. Inspect the captured fixture for sanity.
5. Commit both the test and the fixture.
