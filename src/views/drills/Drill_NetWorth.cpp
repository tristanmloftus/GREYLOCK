// ---------------------------------------------------------------------------
// Drill_NetWorth.cpp — Task v0.3-2.
// ---------------------------------------------------------------------------
// See Drill_NetWorth.h for the public contract; this file is the FTXUI
// composition.  Layout follows the §3b mockup:
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │  Net Worth Detail              Dashboard > Net Worth                │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │                                                                     │
//   │  Total:  $13,234.56                                                 │
//   │  ###############----------                                          │
//   │                                                                     │
//   │  Per account                                                        │
//   │  ────────                                                            │
//   │  Checking      ****abcd   $1,234.56   Last sync: 2026-04-01         │
//   │  ...                                                                 │
//   │                                                                     │
//   │  By type                                                            │
//   │  ────────                                                            │
//   │  Checking    $1,234.56  ###----  ( 9%)                              │
//   │  ...                                                                 │
//   │                                                                     │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │ [Esc] Back   [j/k] Scroll                                           │
//   └─────────────────────────────────────────────────────────────────────┘
//
// SNAPSHOT DETERMINISM
//   Allocation bars use ASCII `#` and `-` (NOT block characters) so the
//   snapshot fixture stays byte-stable across terminals and FTXUI build
//   variants.  The fixture comparison is byte-for-byte; any unicode
//   widget glyph would risk drift between local and CI.
// ---------------------------------------------------------------------------

#include "Drill_NetWorth.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

#include "../../models/Account.h"
#include "../../models/DataStore.h"
#include "../../models/Transaction.h"
#include "../ViewCommon.h"

namespace tf::views::drills {

namespace {

using ftxui::Element;
using ftxui::Elements;
using ftxui::bold;
using ftxui::border;
using ftxui::color;
using ftxui::dim;
using ftxui::filler;
using ftxui::flex;
using ftxui::hbox;
using ftxui::separator;
using ftxui::text;
using ftxui::vbox;

// ---------------------------------------------------------------------------
// format helpers — kept local so the drill files are self-contained and
// don't accidentally pick up the file-scope helpers ui_net_worth.cpp owns.
// ---------------------------------------------------------------------------
std::string format_currency_drill(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (val < 0) oss << "-$" << (-val);
    else         oss << "$" << val;
    return oss.str();
}

std::string format_percent(double pct) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << pct << "%";
    return oss.str();
}

// Right-pad a string to `width` chars with spaces.  Used for table column
// alignment.  No truncation (we trust the caller to keep strings short).
std::string pad_right(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// ASCII allocation bar.  Renders `#` for the filled portion and `-` for
// the remainder.  `pct` is clamped to [0, 100]; `width` is the total
// number of cells.  Returns a plain std::string for compose-via-text().
std::string ascii_bar(double pct, std::size_t width) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    const std::size_t filled = static_cast<std::size_t>(
        std::round((pct / 100.0) * static_cast<double>(width)));
    std::string out;
    out.reserve(width);
    for (std::size_t i = 0; i < filled; ++i) out.push_back('#');
    for (std::size_t i = filled; i < width; ++i) out.push_back('-');
    return out;
}

// Account type -> human label.  Avoids touching Account::type_to_string()
// (which yields "credit_card" with the underscore for serialization).
std::string type_label(AccountType t) {
    switch (t) {
        case AccountType::Checking:   return "Checking";
        case AccountType::Savings:    return "Savings";
        case AccountType::CreditCard: return "Credit";
        case AccountType::Investment: return "Investment";
        case AccountType::Cash:       return "Cash";
        default:                      return "Other";
    }
}

// Last-4 mask helper.  Real account IDs are uuids; v0.3 keeps the
// existing convention of showing only the trailing four characters
// prefixed by ****.  Short IDs (<4 chars) render as **** + the ID
// verbatim so the column never collapses.
std::string mask_id(const std::string& id) {
    if (id.size() <= 4) return "****" + id;
    return "****" + id.substr(id.size() - 4);
}

// Breadcrumb row.  "Dashboard > Net Worth" with the Cyan separator
// (Appendix C.4).  The last segment is bold + emphasized so it reads as
// "you are here".
Element render_breadcrumb() {
    return hbox({
        text("Dashboard") | dim,
        text(" > ") | color(kTokens.accent_info),
        text("Net Worth") | bold | color(kTokens.fg_emphasized),
    });
}

}  // namespace

Drill_NetWorth::Drill_NetWorth(DataStore& data, std::string entity_id)
    : data_(data),
      entity_id_(std::move(entity_id)) {}

ftxui::Element Drill_NetWorth::render() const {
    // ----------------------------------------------------------------------
    // Step 1: collect the accounts in scope.  entity_id_ empty means all.
    // We copy pointers (not Accounts) to keep allocation minimal — the
    // DataStore vector outlives this render call by construction.
    // ----------------------------------------------------------------------
    std::vector<const Account*> accounts;
    if (entity_id_.empty()) {
        for (const auto& a : data_.accounts) accounts.push_back(&a);
    } else {
        // DataStore::get_accounts_for_entity returns non-const pointers;
        // we narrow back to const for our read-only render path.
        for (auto* p : data_.get_accounts_for_entity(entity_id_)) {
            accounts.push_back(p);
        }
    }

    // ----------------------------------------------------------------------
    // Step 2: empty-state branch.  Render a single dim line and stop.
    // ----------------------------------------------------------------------
    if (accounts.empty()) {
        Element body = vbox({
            text(""),
            text("  No accounts linked.") | dim,
            filler(),
        });
        Element panel = vbox({
            hbox({ text("  "), render_breadcrumb() }),
            separator(),
            body,
            separator(),
            text("  [Esc] Back   [j/k] Scroll") | dim,
        }) | border;
        return panel | flex;
    }

    // ----------------------------------------------------------------------
    // Step 3: aggregates needed for the headline + by-type breakdown.
    //   total      — signed sum of account balances.
    //   by_type    — map AccountType -> running balance.
    //   alloc_base — sum of POSITIVE contributions only.  Used as the
    //                denominator for the allocation bar so that a
    //                negative credit card doesn't poison the percent
    //                math (otherwise an account with -500 and +10000
    //                would produce 105% Investment).
    // ----------------------------------------------------------------------
    double total = 0.0;
    std::map<AccountType, double> by_type;
    double alloc_base = 0.0;
    for (const auto* a : accounts) {
        total += a->balance;
        by_type[a->type] += a->balance;
        if (a->balance > 0.0) alloc_base += a->balance;
    }

    // ----------------------------------------------------------------------
    // Step 4: derive per-account last-sync stamp from DataStore::
    // transactions (newest tx.date per account_id, lex-compare on
    // "YYYY-MM-DD" == chronological).  Same pattern Dashboard's
    // ui_sync_status uses at institution granularity.
    // ----------------------------------------------------------------------
    std::map<std::string, std::string> acct_last_sync;
    for (const auto& tx : data_.transactions) {
        auto& cell = acct_last_sync[tx.account_id];
        if (tx.date > cell) cell = tx.date;
    }

    // ----------------------------------------------------------------------
    // Step 5: build the FTXUI element graph.  Layout sections separated
    // by blank rows for breathing room; the byte snapshot tolerates this
    // exactly because all whitespace is part of the captured render.
    // ----------------------------------------------------------------------
    Elements rows;

    // Headline block: "Total: $X.XX" + an allocation bar (60 cells wide).
    // The bar shows the largest single by-type bucket as a sanity glance;
    // detailed per-type bars come below.
    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  Total:  ") | dim,
        text(format_currency_drill(total))
            | bold | color(total >= 0 ? kTokens.accent_positive
                                       : kTokens.accent_negative),
    }));

    // Headline allocation bar: which AccountType dominates positive value?
    // We size by Investment when present (the v0.2 most-common path), else
    // by whatever positive bucket is largest.  This is a glance metric.
    {
        double dominant_pct = 0.0;
        if (alloc_base > 0.0) {
            double dominant = 0.0;
            for (const auto& [_, v] : by_type) {
                if (v > dominant) dominant = v;
            }
            dominant_pct = (dominant / alloc_base) * 100.0;
        }
        rows.push_back(hbox({
            text("  "),
            text(ascii_bar(dominant_pct, 60)) | dim,
        }));
    }
    rows.push_back(text(""));

    // Per-account table.
    rows.push_back(text("  Per account") | bold);
    rows.push_back(text("  ") | dim);
    rows.push_back(hbox({
        text("  "),
        text(std::string(70, '-')) | dim,
    }));
    for (const auto* a : accounts) {
        // Last sync stamp — fall back to "—" when no tx hit this account.
        std::string ls = "—";
        auto it = acct_last_sync.find(a->id);
        if (it != acct_last_sync.end() && !it->second.empty()) {
            ls = "Last sync: " + it->second;
        } else {
            ls = "Last sync: —";
        }

        Element row = hbox({
            text("  "),
            text(pad_right(a->name, 22)),
            text(pad_right(mask_id(a->id), 12)) | dim,
            text(pad_right(format_currency_drill(a->balance), 14))
                | (a->balance < 0 ? color(kTokens.accent_negative)
                                  : color(kTokens.fg_default)),
            text(pad_right(ls, 24)) | dim,
            text(a->institution) | dim,
        });
        rows.push_back(row);
    }
    rows.push_back(text(""));

    // By-type breakdown with proportional bars.
    rows.push_back(text("  By type") | bold);
    rows.push_back(hbox({
        text("  "),
        text(std::string(70, '-')) | dim,
    }));
    // Render in a stable order matching the §3b mockup.
    static const AccountType kTypeOrder[] = {
        AccountType::Checking,
        AccountType::Savings,
        AccountType::CreditCard,
        AccountType::Investment,
        AccountType::Cash,
        AccountType::Other,
    };
    for (AccountType t : kTypeOrder) {
        auto it = by_type.find(t);
        if (it == by_type.end()) continue;
        const double v = it->second;
        const double pct = (alloc_base > 0.0 && v > 0.0)
            ? (v / alloc_base) * 100.0
            : 0.0;
        const std::string label = pad_right(type_label(t), 12);
        const std::string amount = pad_right(format_currency_drill(v), 14);
        // Credit card with negative balance renders the "(debit)" marker
        // in place of a percent — bars don't represent negatives.
        if (t == AccountType::CreditCard && v < 0.0) {
            rows.push_back(hbox({
                text("  "),
                text(label),
                text(amount) | color(kTokens.accent_negative),
                text("  "),
                text(std::string(20, '-')) | dim,
                text("  (debit)") | dim,
            }));
        } else {
            rows.push_back(hbox({
                text("  "),
                text(label),
                text(amount),
                text("  "),
                text(ascii_bar(pct, 20)) | dim,
                text("  (") | dim,
                text(format_percent(pct)),
                text(")") | dim,
            }));
        }
    }

    // Fill remaining vertical space so the bottom hint line sits at the
    // canvas floor.
    rows.push_back(filler());

    Element panel = vbox({
        hbox({ text("  "), render_breadcrumb() }),
        separator(),
        vbox(std::move(rows)),
        separator(),
        text("  [Esc] Back   [j/k] Scroll") | dim,
    }) | border;
    return panel | flex;
}

}  // namespace tf::views::drills
