#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

Component ShovelScore(double score, int supplier_count, double total_shovel_spend);

Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend);

} // namespace ftxui