#pragma once

// ---------------------------------------------------------------------------
// Drill_NetWorth — full-screen drill view for the Net Worth widget.
// ---------------------------------------------------------------------------
// Task v0.3-2.  Replaces the Dashboard render until Esc returns.  Shows:
//   1. Breadcrumb row "Dashboard > Net Worth" (Cyan separators per
//      docs/UI_REDESIGN_V0.3.md §C.4).
//   2. Total net worth headline + horizontal allocation bar showing the
//      relative weight of each AccountType.
//   3. Per-account table (name + masked id + balance + last-sync stamp +
//      institution) for every account contributing to the headline.
//   4. By-type breakdown rows (Checking / Savings / Credit / Investment)
//      each with a proportional bar and percent label.
//   5. Bottom hints row "[Esc] Back   [j/k] Scroll".
//
// DATA SOURCES
//   - DataStore::accounts (filtered to entity_id_ when set).
//   - DataStore::transactions — used ONLY to derive per-account last-sync
//     date (newest tx.date per account_id) so the table doesn't need a
//     new BackendClient method.  Same pattern Dashboard's ui_sync_status
//     uses for institution-level last-sync.
//
// EMPTY STATE
//   When the entity has zero accounts (DataStore::accounts is empty for
//   the entity) the view renders a single line "No accounts linked."
//   inside the body.  The bar / table / breakdown blocks are skipped.
//
// SCROLLING (v0.3-2 scope)
//   The drill body fits within 40 rows for the typical dataset (~10
//   accounts).  A scroll offset is reserved on the public surface but
//   not yet driven; set_scroll_offset() is a no-op visually today.
//
// CALLERS
//   src/main.cpp — App owns the Drill view; renders it instead of
//   DashboardView when FocusController::level() == FocusLevel::Drill and
//   FocusController::drilled_widget() == WidgetId::NetWorth.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3b "Drill 1 — Net Worth" (mockup).
//   docs/UI_REDESIGN_V0.3.md Appendix D (wire-up sketch).
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>
#include <string>

class DataStore;

namespace tf::views::drills {

class Drill_NetWorth {
public:
    // Construct against a DataStore reference + the active entity scope.
    // entity_id can be empty meaning "all entities".
    Drill_NetWorth(DataStore& data, std::string entity_id);

    // Render the drill at the full 120x40 canvas.  Pure: no I/O, no
    // mutation of the DataStore.
    ftxui::Element render() const;

    // Scroll affordance reservation.  v0.3-2 doesn't visually scroll
    // (the body fits inside 40 rows for the sample data), but the
    // setter is on the public surface so main.cpp can wire j/k without
    // an ABI break when v0.3-3 makes scrolling functional.
    void set_scroll_offset(int offset) { scroll_offset_ = offset; }
    int  scroll_offset() const         { return scroll_offset_; }

private:
    DataStore&  data_;
    std::string entity_id_;
    int         scroll_offset_ = 0;
};

}  // namespace tf::views::drills
