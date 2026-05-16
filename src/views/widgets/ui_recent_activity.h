#pragma once

// ---------------------------------------------------------------------------
// ui_recent_activity — Dashboard "Recent Activity" panel renderer.
// ---------------------------------------------------------------------------
// One of Rory's four canonical dashboard widgets (greylock-spec.md §8.3,
// Q3 confirmed).  Shows the last 5 transactions across the visible
// entities, most-recent first.
//
// PARAMETERS
//   rows     A vector of RecentTx POD entries, already sorted desc by
//            date and trimmed to at most 5.
//   focused  Yellow border + bright bold title when true.
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace tf::widgets {

struct RecentTx {
    std::string date;          // "YYYY-MM-DD"
    std::string description;   // merchant / counterparty
    double      amount{0.0};   // signed; positive = inflow, negative = outflow
};

}  // namespace tf::widgets

namespace ftxui {

Element RecentActivityRenderer(const std::vector<tf::widgets::RecentTx>& rows,
                               bool focused = false);

}  // namespace ftxui
