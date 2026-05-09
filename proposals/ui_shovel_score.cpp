#include "ui_shovel_score.h"
#include <sstream>
#include <iomanip>

namespace ftxui {

Component ShovelScore(double score, int supplier_count, double total_shovel_spend) {
    return Renderer([=] {
        return ShovelScoreRenderer(score, supplier_count, total_shovel_spend);
    });
}

Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend) {
    std::vector<Element> rows;

    rows.push_back(text("Shovel Score") | bold);
    rows.push_back(separator());

    std::string label;
    Color score_color;
    if (score >= 80) {
        label = "AI POWERHOUSE";
        score_color = Color::Cyan;
    } else if (score >= 60) {
        label = "EARLY ADOPTER";
        score_color = Color::Green;
    } else if (score >= 40) {
        label = "BUILDING STACK";
        score_color = Color::Yellow;
    } else if (score >= 20) {
        label = "GETTING STARTED";
        score_color = Color::Yellow;
    } else {
        label = "WAITING TO DIG";
        score_color = Color::Gray;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0);
    oss << "  " << score << "/100";

    std::ostringstream oss2;
    oss2 << std::fixed << std::setprecision(2);
    oss2 << "  $" << total_shovel_spend << " to " << supplier_count << " shovel companies";

    rows.push_back(text(oss.str()) | bold | color(score_color));
    rows.push_back(text(label) | color(score_color));
    rows.push_back(text(""));
    rows.push_back(text("Total shovel spend: $" + std::to_string(total_shovel_spend)) | dim);
    rows.push_back(text("Shovel companies: " + std::to_string(supplier_count)) | dim);

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui