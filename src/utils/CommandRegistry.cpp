// ---------------------------------------------------------------------------
// CommandRegistry.cpp — the 18-entry command table + fuzzy lookup.
// ---------------------------------------------------------------------------
// See CommandRegistry.h for the contract and motivation.  The table
// itself is the authoritative declaration order; reordering is a
// breaking change to the ABI-stable CommandId enum.
//
// Score semantics for fuzzy_find:
//   - `name` and `alias` are both fuzzy-matched; the better score wins.
//   - Boolean-only matches (would-be score below zero after penalties)
//     are still included — the user's typed pattern is the source of
//     truth, the algorithm only RANKS, it doesn't reject ambiguous
//     matches.  We let the user see the result and refine.
//   - Ties broken by declaration index ascending; this is a deliberate
//     stability choice that lets us swap match results in/out without
//     reshuffling the UI on identical keystrokes.
// ---------------------------------------------------------------------------

#include "CommandRegistry.h"

#include "fuzzy_match.h"

#include <algorithm>
#include <utility>

namespace tf::utils {

namespace {

// ---------------------------------------------------------------------------
// The published 18 commands.  Reading order matches the doc table in
// docs/UI_REDESIGN_V0.3.md §3c.  Aliases are the short tokens we expect
// users to type at the palette ("dashboard", "tx", "nw", etc.).
// ---------------------------------------------------------------------------
const std::array<Command, kCommandCount> kCommands = {{
    // View switching ------------------------------------------------------
    { CommandId::SwitchView_Dashboard,
      "Switch view: Dashboard",          "dashboard",     "Tab" },
    { CommandId::SwitchView_Accounts,
      "Switch view: Accounts",           "accounts",      "Tab" },
    { CommandId::SwitchView_Transactions,
      "Switch view: Transactions",       "tx",            "Tab" },
    { CommandId::SwitchView_Budget,
      "Switch view: Budget",             "budget",        "Tab" },

    // Entity switching ----------------------------------------------------
    { CommandId::SwitchEntity_Personal,
      "Switch entity: Personal",         "personal",      "1"   },
    { CommandId::SwitchEntity_Business,
      "Switch entity: Business",         "business",      "2"   },

    // Plaid + config ------------------------------------------------------
    { CommandId::LinkPlaid,
      "Link Plaid account",              "plaid-link",    ""    },
    { CommandId::LinkPlaidTest,
      "Link Plaid sandbox (test)",       "plaid-test",    ""    },

    { CommandId::OpenConfig,
      "Open config",                     "config",        ""    },
    { CommandId::Quit,
      "Quit (save and exit)",            "quit",          "q"   },

    // Drill-into commands -------------------------------------------------
    { CommandId::DrillInto_NetWorth,
      "Drill into: Net Worth",           "net-worth",     ""    },
    { CommandId::DrillInto_SyncStatus,
      "Drill into: Sync Status",         "sync-status",   ""    },
    { CommandId::DrillInto_CategoryTrends,
      "Drill into: Category Trends",     "trends",        ""    },

    // Help + session ------------------------------------------------------
    { CommandId::Help,
      "Show keybindings (help)",         "help",          "?"   },
    { CommandId::Logout,
      "Logout (clear session)",          "logout",        ""    },
    { CommandId::Whoami,
      "Show current user (whoami)",      "whoami",        ""    },
    { CommandId::Refresh,
      "Refresh data from backend",       "refresh",       "r"   },

    // Reserved: search box wiring lands in a later v0.3 task; the entry
    // is published here so the palette already knows about it.
    { CommandId::Search_Transactions,
      "Search transactions",             "search",        "/"   },
}};

}  // namespace

// ---------------------------------------------------------------------------
// Static accessor.  Returns a reference to the program-lifetime table.
// ---------------------------------------------------------------------------
const std::array<Command, kCommandCount>& all_commands() {
    return kCommands;
}

// ---------------------------------------------------------------------------
// fuzzy_find — see header for contract.
// ---------------------------------------------------------------------------
std::vector<int> fuzzy_find(std::string_view query, int max_results) {
    if (max_results <= 0) return {};

    // Empty query: pre-sorted declaration order.  We hand back the first
    // N indices unchanged — the palette renders them as the "default
    // suggestions" before any typing.
    if (query.empty()) {
        std::vector<int> out;
        const int cap = std::min(max_results, static_cast<int>(kCommandCount));
        out.reserve(static_cast<std::size_t>(cap));
        for (int i = 0; i < cap; ++i) out.push_back(i);
        return out;
    }

    // Pair (score, index) so std::sort can rank by score desc, index asc.
    struct Hit {
        int score;
        int index;
    };
    std::vector<Hit> hits;
    hits.reserve(kCommandCount);

    for (std::size_t i = 0; i < kCommandCount; ++i) {
        int score_name  = 0;
        int score_alias = 0;
        const bool n = fuzzy_match(query, kCommands[i].name,  score_name);
        const bool a = fuzzy_match(query, kCommands[i].alias, score_alias);
        if (!n && !a) continue;

        // Take the better of the two; alias matches usually beat name
        // matches for short queries (it's the literal command token),
        // but the algorithm decides via score directly.
        int best = 0;
        if (n && a) best = std::max(score_name, score_alias);
        else if (n) best = score_name;
        else        best = score_alias;

        hits.push_back({ best, static_cast<int>(i) });
    }

    // Score desc, then index asc.
    std::sort(hits.begin(), hits.end(),
              [](const Hit& lhs, const Hit& rhs) {
                  if (lhs.score != rhs.score) return lhs.score > rhs.score;
                  return lhs.index < rhs.index;
              });

    const int cap = std::min(max_results, static_cast<int>(hits.size()));
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(cap));
    for (int i = 0; i < cap; ++i) out.push_back(hits[i].index);
    return out;
}

}  // namespace tf::utils
