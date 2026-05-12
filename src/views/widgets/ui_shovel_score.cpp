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
// COLOR DISCIPLINE + TIER LABELS (semantics for THIS widget) — v0.3-5 migrated
//   The score-to-tier mapping is the v0.2 marketing taxonomy:
//     score >= 80 -> "AI POWERHOUSE"   (kTokens.accent_info,     cyan)
//     score >= 60 -> "EARLY ADOPTER"   (kTokens.accent_positive, green)
//     score >= 40 -> "BUILDING STACK"  (kTokens.accent_warning,  yellow)
//     score >= 20 -> "GETTING STARTED" (kTokens.accent_warning,  yellow)
//     score <  20 -> "WAITING TO DIG"  (Color::GrayLight, no semantic token
//                                       maps to GrayLight; this is a neutral
//                                       "not engaged" indicator and is
//                                       intentionally distinct from fg_dim
//                                       (GrayDark) used for metadata).
//
//   Color is informational (a glance-level "where am I"), NOT a
//   warning/error indicator: gray-light means "not engaged with AI
//   infrastructure" and is neutral, not bad.
//
//   SEMANTIC NOTE (v0.3-5): `accent_info` (Cyan) is shared with
//   timestamp-style metadata elsewhere; here it carries a different
//   meaning (top-tier celebratory).  Documented for the integration
//   manager.  Reconciliation candidate for v0.4.
//
//   Thresholds (80/60/40/20) are unanchored magic numbers — they were
//   chosen for demo readability, not because the underlying score is
//   calibrated.  TODO(shovel-score) re-anchor when the scorer is real.
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

#include "../ViewCommon.h"

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
Element ShovelScoreRenderer(double score, int supplier_count, double total_shovel_spend, bool focused) {
    std::vector<Element> rows;

    // Title: bright bold + focus color when focused, plain bold otherwise.
    Element title = text("Shovel Score") | bold;
    if (focused) title = title | color(kTokens.fg_emphasized);
    rows.push_back(title);
    rows.push_back(separator());

    // Tier-label + color lookup; see file header for the taxonomy.
    // Magic numbers are intentional v0.2 demo thresholds; tracked as
    // TODO(shovel-score) — re-anchor when the scorer is real.
    std::string label;
    Color score_color;
    if (score >= 80) {
        label = "AI POWERHOUSE";
        score_color = kTokens.accent_info;
    } else if (score >= 60) {
        label = "EARLY ADOPTER";
        score_color = kTokens.accent_positive;
    } else if (score >= 40) {
        label = "BUILDING STACK";
        score_color = kTokens.accent_warning;
    } else if (score >= 20) {
        label = "GETTING STARTED";
        score_color = kTokens.accent_warning;
    } else {
        label = "WAITING TO DIG";
        // No semantic token maps to GrayLight; "neutral / not engaged"
        // is distinct from `fg_dim` (GrayDark) which is metadata.
        score_color = Color::GrayLight;
    }

    rows.push_back(text("  " + format_score(score) + "/100") | bold | color(score_color));
    rows.push_back(text(label) | color(score_color));
    rows.push_back(text(""));
    rows.push_back(text("Total shovel spend: $" + format_amount(total_shovel_spend)) | dim);
    rows.push_back(text("Shovel companies: " + std::to_string(supplier_count)) | dim);

    Element panel = vbox(std::move(rows));
    if (focused) {
        panel = panel | borderStyled(ROUNDED) | color(kTokens.focus);
    } else {
        panel = panel | border;
    }
    return panel;
}

} // namespace ftxui
