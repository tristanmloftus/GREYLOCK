#pragma once

// ---------------------------------------------------------------------------
// CommandRegistry — data-only command table for the v0.3-4 palette.
// ---------------------------------------------------------------------------
// 20 entries, one per published command in docs/UI_REDESIGN_V0.3.md §3c.
// The registry is INTENTIONALLY data-only:
//
//   - Each Command carries a stable CommandId, a human-readable `name`
//     (what the palette renders), a single-word `alias` (the short token
//     the user is most likely to type — fed as the secondary fuzzy-match
//     surface), and an optional `shortcut` (right-aligned hint, e.g.
//     "Tab" or "q"; empty for commands without a global hotkey).
//   - Invocation lives in main.cpp's dispatch(CommandId) switch.  The
//     registry must NEVER do work that mutates app state — keeping it
//     data-only means tests can construct + query it with zero side
//     effects and no DI plumbing.
//
// FUZZY MATCHING
//   fuzzy_find(query) returns indices into all_commands() ordered by
//   the fts_fuzzy_match score (higher = better) of the query against
//   `name` OR `alias`, whichever scores higher.  Ties are broken by
//   declaration order (i.e. the index value itself, ascending).
//
//   An empty query returns the first `max_results` indices in
//   declaration order — the "default state" of the palette before the
//   user has typed anything.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3c "Command palette"
//   src/utils/fuzzy_match.h (the algorithm)
//   src/views/CommandPalette.{h,cpp} (the renderer + selection model)
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace tf::utils {

// ---------------------------------------------------------------------------
// Stable command IDs.  Order matches the registry array declaration in
// CommandRegistry.cpp.  IDs are persisted indirectly (a user's typed
// alias resolves to one of these at dispatch time); inserting a new ID
// at the end is ABI-safe, reordering is NOT.
// ---------------------------------------------------------------------------
enum class CommandId : int {
    SwitchView_Dashboard          = 0,
    SwitchView_Accounts           = 1,
    SwitchView_Transactions       = 2,
    SwitchView_Budget             = 3,

    SwitchEntity_Personal         = 4,
    SwitchEntity_Business         = 5,

    LinkPlaid                     = 6,
    LinkPlaidTest                 = 7,

    OpenConfig                    = 8,
    Quit                          = 9,

    DrillInto_NetWorth            = 10,
    DrillInto_SyncStatus          = 11,
    DrillInto_CategoryTrends      = 12,

    Help                          = 13,
    Logout                        = 14,
    Whoami                        = 15,
    Refresh                       = 16,

    Search_Transactions           = 17,  // reserved; opens Transactions
                                         // view + focuses search box (the
                                         // search box itself lands in a
                                         // later v0.3 task).

    // Appended (greylock-spec.md v2): added at the end so existing IDs
    // stay stable — see header note "inserting at the end is ABI-safe,
    // reordering is NOT".
    SwitchView_Categories         = 18,

    // v3 + v4 scaffolds (greylock-spec.md §8.12–§8.18).
    SwitchView_Notes              = 19,
    SwitchView_Decisions          = 20,
    SwitchView_Tasks              = 21,
    SwitchView_Events             = 22,
    SwitchView_Proposals          = 23,
    SwitchView_Targets            = 24,
    SwitchView_Relationships      = 25,
    SwitchView_RealEstate         = 26,
};

// ---------------------------------------------------------------------------
// A single palette entry.  Pointer-to-literal strings; the registry is
// static storage so lifetimes are program-long.
// ---------------------------------------------------------------------------
struct Command {
    CommandId       id;
    const char*     name;       // "Switch view: Dashboard"
    const char*     alias;      // "dashboard"  (secondary fuzzy surface)
    const char*     shortcut;   // "Tab" or "" if no global hotkey.
};

// ---------------------------------------------------------------------------
// The full registry, in declaration order.  Size is 27 (v1 + v2 + v3/v4
// scaffolds).  Inserting a new ID at the END is ABI-safe; reordering
// existing IDs is NOT.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kCommandCount = 27;

const std::array<Command, kCommandCount>& all_commands();

// ---------------------------------------------------------------------------
// fuzzy_find — score-ranked command lookup.
//
// Returns indices into all_commands() ordered by best fts_fuzzy_match
// score (higher first).  Each entry is scored against its `name` and
// its `alias`; the better of the two scores wins.  Entries whose
// `name` AND `alias` both fail the fuzzy_match boolean test are
// excluded.  Ties are broken by declaration order ascending (so an
// earlier-declared command outranks a later one at the same score).
//
// `max_results` caps the returned vector size.  Default 8 matches the
// palette overlay's render budget; smaller values are accepted (tests
// use 3 or 4).
//
// Empty `query`: returns the first `max_results` indices in declaration
// order.  This is the palette's initial state before any typing.
// ---------------------------------------------------------------------------
std::vector<int> fuzzy_find(std::string_view query, int max_results = 8);

}  // namespace tf::utils
