#pragma once

// ---------------------------------------------------------------------------
// Drill_SyncStatus — full-screen drill view for the Sync Status widget.
// ---------------------------------------------------------------------------
// Task v0.3-3.  Replaces the Dashboard render until Esc returns.  Shows:
//   1. Breadcrumb row "Dashboard > Sync Status" (Cyan separators per
//      docs/UI_REDESIGN_V0.3.md §C.4).
//   2. Title in bold.
//   3. Per-institution blocks.  Each block shows the Item id, last-sync
//      timestamp, status (OK / AUTH_ERROR / etc.), and an indented list
//      of the accounts that Item owns with masked id + last-tx date.
//   4. A subtle banner "Server endpoint not yet available; showing local
//      cache." when BackendClient::get_sync_status() returned nullopt
//      (i.e. the server responded 404 — the v0.4 endpoint hasn't shipped
//      yet).  In that fallback mode the drill renders the same data the
//      dashboard widget uses (newest-tx-date-per-institution from
//      DataStore) so the user still sees their connection map.
//   5. Bottom hints row.  When `server_available` is true, the row reads
//      "[Esc] Back   [j/k] Scroll   [r] Refresh   [R] Re-auth"; when
//      false (fallback mode), the [R] hint is replaced with
//      "[R] Re-auth (coming v0.4)" so the affordance is visible but the
//      user knows why the key won't fire.
//
// DATA SOURCES
//   Real path:
//     BackendClient::get_sync_status() returns Optional<vector<
//     SyncStatusItem>>.  When the optional is engaged, each Item row
//     becomes one block in the render.  The drill view itself does NOT
//     call BackendClient — main.cpp performs the call and passes the
//     parsed vector + a sentinel for the 404 case to the constructor.
//     Keeping the I/O outside the view leaves the drill pure (no FTXUI
//     test needs an HTTP mock).
//
//   Fallback path (server returned 404):
//     DataStore::accounts grouped by acc.institution + DataStore::
//     transactions used to derive the newest-tx date per institution
//     (same helper the dashboard widget ui_sync_status uses).  No
//     per-Item granularity is available — the block renders the
//     institution-level summary only.
//
// EMPTY STATE
//   When neither source yields any institutions (zero linked accounts
//   AND server returned an empty array), the body shows a single dim
//   line "No institutions linked." inside the panel.  Bottom hint row
//   stays unchanged so the user can Esc out.
//
// FEATURE FLAG GATING
//   The [R] re-auth key (handled in main.cpp's CatchEvent) is enabled
//   only when `server_available` is true (constructor parameter).
//   Drill_SyncStatus does NOT consume the R event — it only renders the
//   hint string; main.cpp gates the actual dispatch.
//
// CALLERS
//   src/main.cpp — App owns the Drill view; renders it instead of
//   DashboardView when FocusController::level() == FocusLevel::Drill and
//   FocusController::drilled_widget() == WidgetId::SyncStatus.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3b "Drill 3 — Sync Status" (mockup).
//   docs/UI_REDESIGN_V0.3.md § Task v0.3-3 (server fallback contract).
//   src/services/BackendClient.h (struct SyncStatusItem).
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>
#include <optional>
#include <string>
#include <vector>

#include "../../services/BackendClient.h"  // SyncStatusItem

class DataStore;

namespace tf::views::drills {

class Drill_SyncStatus {
public:
    // Construct against a DataStore reference + the per-Item server
    // response.  Pass std::nullopt for `server_items` when the server
    // returned 404 (the drill will render the banner + DataStore
    // fallback).  Pass an engaged-but-empty vector when the server is
    // available but the user has no linked Items yet (the drill renders
    // the empty-state line instead of the fallback banner).
    Drill_SyncStatus(DataStore& data,
                     std::optional<std::vector<SyncStatusItem>> server_items);

    // Render the drill at the full 120x40 canvas.  Pure: no I/O, no
    // mutation of the DataStore.
    ftxui::Element render() const;

    // True when the server returned a real response (engaged optional).
    // main.cpp reads this to decide whether the [R] re-auth key is
    // active for the current drill render.
    bool server_available() const noexcept { return server_items_.has_value(); }

    // Scroll affordance reservation; see Drill_NetWorth.h for the same
    // contract.  v0.3-3 does not visually scroll (the body fits inside
    // 40 rows for the v0.2 sample dataset).
    void set_scroll_offset(int offset) { scroll_offset_ = offset; }
    int  scroll_offset() const         { return scroll_offset_; }

private:
    DataStore&                                  data_;
    std::optional<std::vector<SyncStatusItem>>  server_items_;
    int                                         scroll_offset_ = 0;
};

}  // namespace tf::views::drills
