#pragma once

// ---------------------------------------------------------------------------
// ui_sync_status — Dashboard "Connection Status" panel renderer.
// ---------------------------------------------------------------------------
// Renders one row per linked institution showing whether the connection
// is healthy, when it was last synced, and (if disconnected) the most
// recent error message.
//
// PARAMETERS
//   statuses   A vector of SyncStatus POD entries.  Caller is responsible
//              for ordering (DashboardView sorts by institution name
//              ascending so the panel is stable across renders).
//
// DATA-SOURCE CAVEAT
//   In v0.2 the caller derives these entries from DataStore (newest tx
//   per institution + a "has any tx" connection flag) because the
//   BackendClient does not yet expose a real sync_status() method.
//   When that method ships, the caller should switch to it — the
//   widget's contract does not change.  See DashboardView.cpp PANEL 5
//   for the current derivation.
//
// NAMESPACE NOTE
//   SyncStatus lives in `namespace tf::widgets` (Phase 5 isolation of
//   widget-owned PODs).  Render functions stay in `namespace ftxui`.
//
// CALLERS
//   src/views/DashboardView.cpp (the only caller today).
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

// One institution's sync state.
//
// FIELDS
//   institution     Human-readable institution name ("Chase", "Vanguard").
//                   Used verbatim as the row label.
//   connected       True if the institution is currently considered
//                   healthy.  In v0.2 this is inferred from "has any
//                   transactions"; in v0.3 it will come from a real
//                   server-side sync state.
//   last_sync       "YYYY-MM-DD" of the newest transaction the TUI has
//                   seen for this institution (v0.2 stand-in for "last
//                   server sync").  Empty string == never seen.
//   error_message   When `connected` is false, an optional short error
//                   string shown in place of the timestamp.  Empty
//                   string == no specific error reason ("Never").
struct SyncStatus {
    std::string institution;
    bool connected;
    std::string last_sync;
    std::string error_message;
};

} // namespace tf::widgets

namespace ftxui {

// Component wrapper for container slotting.  Currently unused.
Component SyncStatusIndicator(const std::vector<tf::widgets::SyncStatus>& statuses);

// Build a single-frame FTXUI Element.  Pure function.
Element SyncStatusIndicatorRenderer(const std::vector<tf::widgets::SyncStatus>& statuses);

} // namespace ftxui
