#pragma once

// ---------------------------------------------------------------------------
// DashboardView — the default TUI view.
// ---------------------------------------------------------------------------
// Phase 5 rewrite: this view composes five live panels using the FTXUI
// widgets promoted from proposals/ in commit f573278:
//   - ui_net_worth          (per-account-type balances)
//   - ui_category_trends    (top spending categories, MoM change)
//   - ui_shovel_intelligence (discovered AI-infrastructure suppliers)
//   - ui_shovel_score       (composite 0-100 AI-spend score; v0.2 stop-gap)
//   - ui_sync_status        (per-institution last-sync time + connection)
//
// DATA FLOW:
//   server (SQLCipher-encrypted DB)
//     └─> BackendClient (HTTP, server-side accounts/transactions/categories)
//           └─> DataStore (in-process cache, owned by the TUI process)
//                 └─> DashboardView::render()
//                       └─> widgets in src/views/widgets/*
//
//   This view holds NO BackendClient state directly.  It reads only from
//   DataStore and DiscoveryService.  Sync status falls back to DataStore-
//   derived counts (newest tx per institution) until BackendClient exposes
//   a sync_status() method.
//
// LAYOUT:
//   Two-row responsive grid composed with FTXUI hbox + flex:
//     Top row:    [ NetWorth ][ ShovelScore ][ SyncStatus ]
//     Bottom row: [ ShovelIntelligence ][ CategoryTrends ]
//
// SEE ALSO:
//   V0_2_PLAN.md § Phase 5 — Dashboard live wiring.
//   src/views/widgets/* for the individual panel renderers.
//   src/services/DiscoveryService.h for the shovel-supplier mapping.
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>
#include <string>

#include "FocusController.h"

class DataStore;

// ---------------------------------------------------------------------------
// DashboardView
//
// Holds a non-owning reference to DataStore.  Construction is cheap (no
// allocation).  render() is called once per frame by the TUI driver and
// re-aggregates from the current DataStore snapshot — there is no internal
// memoization, so render cost scales with the number of accounts and
// transactions held in DataStore.
//
// Thread safety: NOT thread-safe.  Caller must serialize render() with any
// DataStore mutation (today this is enforced by the single-threaded TUI
// event loop).
// ---------------------------------------------------------------------------
class DashboardView {
public:
    explicit DashboardView(DataStore& data_store);

    // Scope all aggregations to a specific entity (e.g. household member or
    // PCC vs. personal).  Empty string (the default) means "all entities".
    void set_entity_id(const std::string& id) { entity_id_ = id; }

    // Render the composed dashboard for `current_month`.
    //   current_month: "YYYY-MM" (e.g. "2026-05").  Used to bucket per-month
    //                  category spend and per-ticker shovel velocity.
    //   focus:         (Task v0.3-1) optional read-only focus state.  When
    //                  a widget is reported as focused via
    //                  FocusController::is_widget_focused(), the matching
    //                  panel renders with the yellow focus border and a
    //                  bright title.  A nullptr means "no focus state" and
    //                  preserves the v0.2 visual byte-for-byte.
    ftxui::Element render(const std::string& current_month,
                          const tf::views::FocusController* focus = nullptr);

private:
    DataStore& data_store_;
    std::string entity_id_;
};
