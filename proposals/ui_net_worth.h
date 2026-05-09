#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth);

Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth);

} // namespace ftxui