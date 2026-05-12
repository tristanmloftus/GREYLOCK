#pragma once

// ---------------------------------------------------------------------------
// Drill_ShovelScore — full-screen drill view for the Shovel Score widget.
// ---------------------------------------------------------------------------
// Task v0.3-2.  Explains the 0-100 number on the Dashboard by showing:
//   1. Breadcrumb "Dashboard > Shovel Score" (Cyan separators).
//   2. Headline score panel: "XX/100" + tier label ("EARLY ADOPTER", etc.).
//   3. Formula card: the v0.2 stop-gap explanation (median |MoM velocity|
//      over top-10 suppliers, clamped [0,100]).
//   4. Inputs table: top-10 suppliers by total_spend with their cur/prev
//      month spend and the per-ticker MoM percent change.
//   5. The actual median + clamp calculation rendered as a one-line
//      breadcrumb of how the headline number was reached.
//   6. Bottom hints row "[Esc] Back   [j/k] Scroll".
//
// DATA SOURCE
//   DiscoveryService::aggregate_supplier_spend(transactions, current_month,
//   previous_month) — shared with the Dashboard widget (the whole point
//   of the Task v0.3-2 extraction).  Drill takes the first 10 entries
//   for its "Inputs" table; the headline score is computed from the
//   |percent_change| of those rows using the same median-clamp formula
//   that lives in DashboardView's compute_shovel_score().
//
// EMPTY STATE
//   When aggregate_supplier_spend() returns an empty vector (no
//   transactions, or no merchant matches), the headline renders as
//   "--/100" with the tier "INSUFFICIENT DATA" and a single line
//   instructing the user to link bank accounts.  The Inputs table /
//   formula card are still shown so the user understands what the
//   number would mean once data exists.
//
// CALLERS
//   src/main.cpp — same pattern as Drill_NetWorth.
//
// SEE ALSO
//   docs/UI_REDESIGN_V0.3.md §3b "Drill 2 — Shovel Score" (mockup).
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>
#include <string>

class DataStore;

namespace tf::views::drills {

class Drill_ShovelScore {
public:
    Drill_ShovelScore(DataStore& data,
                      std::string current_month,
                      std::string previous_month);

    // Render the drill at full 120x40 canvas.
    ftxui::Element render() const;

    // Scroll affordance reservation — see Drill_NetWorth.h for the same
    // contract.
    void set_scroll_offset(int offset) { scroll_offset_ = offset; }
    int  scroll_offset() const         { return scroll_offset_; }

private:
    DataStore&  data_;
    std::string current_month_;
    std::string previous_month_;
    int         scroll_offset_ = 0;
};

}  // namespace tf::views::drills
