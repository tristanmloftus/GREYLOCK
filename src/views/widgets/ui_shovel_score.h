// ui_shovel_score — promoted from proposals/ in Phase 5.
//
// Renders the user's "Shovel Score" — a 0-100 composite of how much of
// the user's spend goes to AI-infrastructure suppliers ("shovels"),
// along with the count of distinct suppliers and total shovel spend.

#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace ftxui {

Component ShovelScore(double score, int supplier_count, double total_shovel_spend);

Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend);

} // namespace ftxui
