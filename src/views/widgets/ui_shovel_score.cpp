#include "ui_shovel_score.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {
std::string format_amount(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val;
    return oss.str();
}

std::string format_score(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << val;
    return oss.str();
}
}  // namespace

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
        score_color = Color::GrayLight;
    }

    rows.push_back(text("  " + format_score(score) + "/100") | bold | color(score_color));
    rows.push_back(text(label) | color(score_color));
    rows.push_back(text(""));
    rows.push_back(text("Total shovel spend: $" + format_amount(total_shovel_spend)) | dim);
    rows.push_back(text("Shovel companies: " + std::to_string(supplier_count)) | dim);

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
