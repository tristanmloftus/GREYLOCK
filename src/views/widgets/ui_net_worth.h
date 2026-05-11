// ui_net_worth — promoted from proposals/ in Phase 5.
//
// Pure render function: takes primitive doubles (per-account-type balances
// and a net-worth total) and returns an FTXUI Element/Component.  No I/O,
// no DataStore access — the caller (DashboardView) is responsible for
// aggregating data.

#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth);

Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth);

} // namespace ftxui
