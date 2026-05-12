// ---------------------------------------------------------------------------
// ui_shovel_score.cpp — implementation of the Dashboard "Shovel Score" panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  Header "Shovel Score" (bold) + separator,
//   then a large headline of "<score>/100" (bold, colored), a one-line
//   tier label ("AI POWERHOUSE", etc., same color), a blank spacer, and
//   two dim summary lines for total spend and supplier count.
//
// FORMATTING RULES
//   - score: 0-decimal precision (integer-looking display, e.g. "73/100").
//     Caller is responsible for clamping to [0,100]; this widget does
//     NOT re-clamp or re-round beyond the iostream precision setting.
//   - total_shovel_spend: 2-decimal precision, leading "$".
//   - supplier_count: rendered via std::to_string (no formatting).
//
// COLOR DISCIPLINE + TIER LABELS (semantics for THIS widget)
//   The score-to-tier mapping is the v0.2 marketing taxonomy:
//     score >= 80 -> "AI POWERHOUSE"   (Cyan)
//     score >= 60 -> "EARLY ADOPTER"   (Green)
//     score >= 40 -> "BUILDING STACK"  (Yellow)
//     score >= 20 -> "GETTING STARTED" (Yellow)
//     score <  20 -> "WAITING TO DIG"  (GrayLight)
//
//   Color is informational (a glance-level "where am I"), NOT a
//   warning/error indicator: gray-light means "not engaged with AI
//   infrastructure" and is neutral, not bad.  Cyan at the top end is
//   intentionally distinct from green elsewhere in the dashboard so
//   the headline score does not blur into the net-worth green.
//
//   Thresholds (80/60/40/20) are unanchored magic numbers — they were
//   chosen for demo readability, not because the underlying score is
//   calibrated.  The v0.3 redesign should re-anchor them to a real
//   distribution once the score formula stops being a stop-gap.
//   TODO(shovel-score).
//
// EDGE CASES
//   - score == 0:           "WAITING TO DIG" tier rendered.
//   - score >= 100:         clamps visually at the AI POWERHOUSE tier;
//                           "<score>/100" still displays the raw value
//                           (no defensive cap here).
//   - supplier_count == 0:  renders "Shovel companies: 0" — DOES NOT
//                           swap in an empty-state placeholder.  The
//                           empty state lives on ui_shovel_intelligence
//                           one row down on the dashboard.
//   - total_shovel_spend negative: "Total shovel spend: $-N.NN" — should
//                           not occur given the upstream filter, but no
//                           defensive abs() is applied here.
//
// CALLERS
//   src/views/DashboardView.cpp::render() — once per frame.
// ---------------------------------------------------------------------------

#include "ui_shovel_score.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {

// Format a dollar amount as "N.NN" (no "$" prefix; caller prepends).
std::string format_amount(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val;
    return oss.str();
}

// Format a score as an integer-looking "N" with zero decimal places.
std::string format_score(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << val;
    return oss.str();
}
}  // namespace

// ---------------------------------------------------------------------------
// ShovelScore
// ---------------------------------------------------------------------------
// Component wrapper around the renderer.  Captures inputs by copy so the
// resulting Component can outlive the call site.
// ---------------------------------------------------------------------------
Component ShovelScore(double score, int supplier_count, double total_shovel_spend) {
    return Renderer([=] {
        return ShovelScoreRenderer(score, supplier_count, total_shovel_spend);
    });
}

// ---------------------------------------------------------------------------
// ShovelScoreRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function.  Called once per frame by DashboardView::render().
// ---------------------------------------------------------------------------
Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend) {
    std::vector<Element> rows;

    rows.push_back(text("Shovel Score") | bold);
    rows.push_back(separator());

    // Tier-label + color lookup; see file header for the taxonomy.
    // Magic numbers are intentional v0.2 demo thresholds; tracked as
    // TODO(shovel-score) — re-anchor when the scorer is real.
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
