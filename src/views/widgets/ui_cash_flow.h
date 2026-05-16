#pragma once

// ---------------------------------------------------------------------------
// ui_cash_flow — Dashboard "Cash Flow This Month" panel renderer.
// ---------------------------------------------------------------------------
// One of Rory's four canonical dashboard widgets (greylock-spec.md §8.3,
// Q3 confirmed).  Shows three numbers for the current calendar month:
//   - Income:   sum of positive-amount tx
//   - Expenses: sum of |negative-amount tx|
//   - Net:      income - expenses (green if >= 0, red if negative)
//
// PARAMETERS
//   income, expenses, net   Plain double dollar amounts.  Caller
//                            (DashboardView) aggregates from DataStore.
//   focused                 Yellow border + bright bold title when true.
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>

namespace ftxui {

Element CashFlowThisMonthRenderer(double income,
                                  double expenses,
                                  double net,
                                  bool   focused = false);

}  // namespace ftxui
