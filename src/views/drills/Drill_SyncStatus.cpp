// ---------------------------------------------------------------------------
// Drill_SyncStatus.cpp — Task v0.3-3.
// ---------------------------------------------------------------------------
// See Drill_SyncStatus.h for the public contract; this file is the FTXUI
// composition.  Layout follows the §3b "Drill 3 — Sync Status" mockup with
// the v0.3-3 fallback banner stitched in:
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │  Dashboard > Sync Status                                            │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │                                                                     │
//   │  [Server endpoint not yet available; showing local cache.]          │  <- fallback banner
//   │                                                                     │
//   │  Chase Bank                                                         │
//   │  ├─ Item id: item_abc123  Last sync: 2026-04-01 14:32 UTC  Status: OK
//   │  ├─ Accounts (2):                                                   │
//   │  │  ├─ Chase Checking   ****1234   Last tx: 2026-04-01              │
//   │  │  └─ Chase Sapphire   ****9012   Last tx: 2026-03-31              │
//   │  ...                                                                │
//   │                                                                     │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │ [Esc] Back   [j/k] Scroll   [r] Refresh   [R] Re-auth               │
//   └─────────────────────────────────────────────────────────────────────┘
//
// SNAPSHOT DETERMINISM
//   Last-sync timestamps come from `last_success_unix` (server path) or
//   from "YYYY-MM-DD" transaction dates (fallback path).  The fallback
//   path is the one exercised by the v0.3-3 snapshot — the server path
//   would require a clock source that's hostile to byte-stable tests.
//   When the v0.4 server endpoint ships, the snapshot will be updated to
//   include the server-path render too.
// ---------------------------------------------------------------------------

#include "Drill_SyncStatus.h"

#include <algorithm>
#include <map>
#include <set>
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
// pad_right — right-pad to `width` chars.  Local copy (each drill file is
// self-contained per the v0.3-2 convention).
// ---------------------------------------------------------------------------
std::string pad_right(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// ---------------------------------------------------------------------------
// mask_id — render a last-4 mask for the account id column.
// ---------------------------------------------------------------------------
std::string mask_id(const std::string& id) {
    if (id.size() <= 4) return "****" + id;
    return "****" + id.substr(id.size() - 4);
}

// ---------------------------------------------------------------------------
// status_label — humanize a SyncStatusItem.last_error_code.
// ---------------------------------------------------------------------------
// Empty error_code => "OK".  Anything else passes through verbatim — the
// server contract uses screaming-snake error codes (ITEM_LOGIN_REQUIRED,
// AUTH_ERROR) that read well on screen without remapping.
std::string status_label(const std::string& error_code) {
    if (error_code.empty()) return "OK";
    return error_code;
}

// ---------------------------------------------------------------------------
// is_error_state — true when the row should show a recommended re-auth
// hint and the status text should render in the negative accent color.
// ---------------------------------------------------------------------------
bool is_error_state(const std::string& error_code) {
    return !error_code.empty();
}

// ---------------------------------------------------------------------------
// render_breadcrumb — "Dashboard > Sync Status" with cyan separator and
// the last segment bold + emphasized.
// ---------------------------------------------------------------------------
Element render_breadcrumb() {
    return hbox({
        text("Dashboard") | dim,
        text(" > ") | color(kTokens.accent_info),
        text("Sync Status") | bold | color(kTokens.fg_emphasized),
    });
}

// ---------------------------------------------------------------------------
// last_tx_for_account — newest tx.date for a given account.id.  Lexico-
// graphic comparison on "YYYY-MM-DD" == chronological.
// ---------------------------------------------------------------------------
std::string last_tx_for_account(const DataStore& data,
                                const std::string& account_id) {
    std::string newest;
    for (const auto& tx : data.transactions) {
        if (tx.account_id != account_id) continue;
        if (tx.date > newest) newest = tx.date;
    }
    return newest.empty() ? std::string("—") : newest;
}

// ---------------------------------------------------------------------------
// render_account_subrow — one indented "Account Name   ****id   Last tx:
// YYYY-MM-DD" row inside an Item block.
// ---------------------------------------------------------------------------
Element render_account_subrow(const Account& a, const DataStore& data,
                              bool is_last) {
    const std::string tree = is_last ? "  |  +- " : "  |  +- ";
    // We always use "+- " regardless of position; the §3b mockup uses
    // the same glyph on every row (the rightmost branch decoration was
    // for a richer 3-deep mockup we deliberately did not pull in).
    const std::string ls = last_tx_for_account(data, a.id);
    return hbox({
        text(tree) | dim,
        text(pad_right(a.name, 22)),
        text(pad_right(mask_id(a.id), 12)) | dim,
        text("Last tx: ") | dim,
        text(ls),
    });
}

}  // namespace

Drill_SyncStatus::Drill_SyncStatus(
    DataStore& data,
    std::optional<std::vector<SyncStatusItem>> server_items)
    : data_(data),
      server_items_(std::move(server_items)) {}

ftxui::Element Drill_SyncStatus::render() const {
    // ----------------------------------------------------------------------
    // Step 1: pick render mode.  Two paths:
    //   (a) server path — server_items_ is engaged AND non-empty.  We
    //       render one block per Item, joining each Item's account_ids to
    //       DataStore::accounts for the indented account list.
    //   (b) fallback path — server_items_ is std::nullopt OR engaged-empty.
    //       We render one block per institution derived from
    //       DataStore::accounts, with no per-Item granularity.  The
    //       fallback banner is shown when server_items_ is nullopt; an
    //       engaged-empty vector means "server is up, you just have zero
    //       Items linked" — the empty-state line covers that case.
    // ----------------------------------------------------------------------
    const bool server_has_data =
        server_items_.has_value() && !server_items_->empty();
    const bool show_fallback_banner = !server_items_.has_value();

    Elements rows;
    rows.push_back(text(""));

    // ----------------------------------------------------------------------
    // Step 2: fallback banner.  Subtle, dim-yellow accent to make it
    // visible without screaming.  Skipped when server_items_ is engaged
    // (server is up; either has data or is empty).
    // ----------------------------------------------------------------------
    if (show_fallback_banner) {
        rows.push_back(hbox({
            text("  "),
            text("[Server endpoint not yet available; showing local cache.]")
                | color(kTokens.accent_warning),
        }));
        rows.push_back(text(""));
    }

    // ----------------------------------------------------------------------
    // Step 3a: server path render.  One block per Item.
    // ----------------------------------------------------------------------
    if (server_has_data) {
        const auto& items = *server_items_;

        // Build an account_id -> Account* lookup once so each Item's
        // account_ids list can resolve in O(1) per id.
        std::map<std::string, const Account*> by_id;
        for (const auto& a : data_.accounts) by_id[a.id] = &a;

        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];

            // Header line: bold institution name.
            rows.push_back(hbox({
                text("  "),
                text(it.institution) | bold,
            }));

            // Item id + status line.  status renders in negative accent
            // when the error_code is non-empty, otherwise default.
            const std::string status_text = status_label(it.last_error_code);
            const auto status_color = is_error_state(it.last_error_code)
                ? kTokens.accent_negative
                : kTokens.accent_positive;
            // Last-sync stamp: we render last_success_unix in a deter-
            // ministic short form.  For the v0.3-3 snapshot exercise
            // server_has_data is false (the test exercises the fallback
            // path), so this code path is currently lint-clean only —
            // when v0.4 wires the endpoint we'll snapshot it.
            std::ostringstream oss;
            oss << "  +- Item id: " << it.item_id
                << "  Last sync (unix): " << it.last_success_unix
                << "  Status: ";
            rows.push_back(hbox({
                text(oss.str()) | dim,
                text(status_text) | color(status_color),
            }));

            // Recommended action when the row is in an error state.
            if (is_error_state(it.last_error_code)) {
                rows.push_back(hbox({
                    text("  |  ") | dim,
                    text("Error: ") | color(kTokens.accent_negative),
                    text(it.last_error_code) | dim,
                    text("  -- press [R] to re-auth.") | dim,
                }));
            }

            // Accounts (N) header.
            std::ostringstream cnt;
            cnt << "  +- Accounts (" << it.account_ids.size() << "):";
            rows.push_back(text(cnt.str()) | dim);

            // Per-account sub-rows.
            for (std::size_t k = 0; k < it.account_ids.size(); ++k) {
                auto found = by_id.find(it.account_ids[k]);
                if (found == by_id.end()) continue;
                rows.push_back(render_account_subrow(
                    *found->second, data_,
                    /*is_last=*/(k + 1 == it.account_ids.size())));
            }

            rows.push_back(text(""));
        }
    }
    // ----------------------------------------------------------------------
    // Step 3b: fallback path render.  Group DataStore::accounts by
    // institution name, one block per institution.
    // ----------------------------------------------------------------------
    else {
        // Aggregate: institution name -> vector of Account pointers.
        std::map<std::string, std::vector<const Account*>> by_inst;
        for (const auto& a : data_.accounts) {
            if (a.institution.empty()) continue;
            by_inst[a.institution].push_back(&a);
        }

        // Newest-tx-date per institution (lex-compare on "YYYY-MM-DD"
        // == chronological).  Same pattern the dashboard widget uses.
        std::map<std::string, std::string> inst_newest;
        std::map<std::string, std::string> account_to_inst;
        for (const auto& a : data_.accounts) {
            if (a.institution.empty()) continue;
            account_to_inst[a.id] = a.institution;
        }
        for (const auto& tx : data_.transactions) {
            auto it = account_to_inst.find(tx.account_id);
            if (it == account_to_inst.end()) continue;
            auto& cell = inst_newest[it->second];
            if (tx.date > cell) cell = tx.date;
        }

        if (by_inst.empty()) {
            rows.push_back(text("  No institutions linked.") | dim);
        }

        for (const auto& [inst, accts] : by_inst) {
            // Header line: bold institution name.
            rows.push_back(hbox({
                text("  "),
                text(inst) | bold,
            }));

            // Item id + status line.  In fallback mode we have no real
            // Item id; we show the institution name verbatim and a
            // synthetic "(local)" qualifier so the user sees that the
            // row is derived from DataStore.  Status is "OK" when the
            // institution has any transactions, "NO DATA" otherwise.
            const std::string newest = inst_newest[inst];
            const bool has_tx = !newest.empty();
            const std::string status_text = has_tx ? "OK" : "NO DATA";
            const auto status_color = has_tx ? kTokens.accent_positive
                                             : kTokens.accent_warning;
            std::string ls = has_tx ? newest : std::string("never");

            rows.push_back(hbox({
                text("  +- Item id: ") | dim,
                text("(local cache)") | dim,
                text("  Last sync: ") | dim,
                text(ls),
                text("  Status: ") | dim,
                text(status_text) | color(status_color),
            }));

            // Accounts (N) header.
            std::ostringstream cnt;
            cnt << "  +- Accounts (" << accts.size() << "):";
            rows.push_back(text(cnt.str()) | dim);

            // Per-account sub-rows.
            for (std::size_t k = 0; k < accts.size(); ++k) {
                rows.push_back(render_account_subrow(
                    *accts[k], data_,
                    /*is_last=*/(k + 1 == accts.size())));
            }

            rows.push_back(text(""));
        }
    }

    rows.push_back(filler());

    // ----------------------------------------------------------------------
    // Step 4: bottom hints row.  The [R] hint shows "(coming v0.4)" when
    // server_items_ is nullopt — the visible affordance matches the
    // feature-flag-gated dispatch in main.cpp.
    // ----------------------------------------------------------------------
    const std::string hint = server_items_.has_value()
        ? "  [Esc] Back   [j/k] Scroll   [r] Refresh   [R] Re-auth"
        : "  [Esc] Back   [j/k] Scroll   [r] Refresh   [R] Re-auth (coming v0.4)";

    Element panel = vbox({
        hbox({ text("  "), render_breadcrumb() }),
        separator(),
        vbox(std::move(rows)),
        separator(),
        text(hint) | dim,
    }) | border;
    return panel | flex;
}

}  // namespace tf::views::drills
