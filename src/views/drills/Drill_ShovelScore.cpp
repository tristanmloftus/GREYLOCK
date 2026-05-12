// ---------------------------------------------------------------------------
// Drill_ShovelScore.cpp — Task v0.3-2.
// ---------------------------------------------------------------------------
// See Drill_ShovelScore.h for the public contract.  This file composes
// the FTXUI element graph from the data DiscoveryService::aggregate_
// supplier_spend() returns; it does NOT call compute_shovel_score() from
// DashboardView (that helper is file-local).  Instead the same median-
// clamp formula is reimplemented here against the drill's own slice of
// the data.  Sharing the formula across both call sites is a v0.3-3
// follow-up — see TODO(v0.3-3) below.
//
// LAYOUT (matches §3b mockup):
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │  Shovel Score Detail                  Dashboard > Shovel Score       │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │                                                                     │
//   │      72/100                                                         │
//   │      EARLY ADOPTER                                                  │
//   │                                                                     │
//   │  Score formula (v0.2 stop-gap)                                      │
//   │  ─────────────────────────────                                       │
//   │  median(|MoM velocity|) over top-10 suppliers by spend, clamped [0,100]
//   │                                                                     │
//   │  Inputs                                                             │
//   │  ─────────────────────────────                                       │
//   │  NVDA   $2,500.00   cur $1,200.00   prev $545.00   MoM +120.0%      │
//   │  ...                                                                 │
//   │                                                                     │
//   │  Median |MoM|: 12.5%                                                │
//   │  Clamped (x100): 72.0 -> 72                                          │
//   │                                                                     │
//   ├─────────────────────────────────────────────────────────────────────┤
//   │ [Esc] Back   [j/k] Scroll                                           │
//   └─────────────────────────────────────────────────────────────────────┘
// ---------------------------------------------------------------------------

#include "Drill_ShovelScore.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

#include "../../models/DataStore.h"
#include "../../models/Transaction.h"
#include "../../services/DiscoveryService.h"
#include "../ViewCommon.h"

namespace tf::views::drills {

namespace {

using ftxui::Element;
using ftxui::Elements;
using ftxui::bold;
using ftxui::border;
using ftxui::color;
using ftxui::dim;
using ftxui::filler;
using ftxui::flex;
using ftxui::hbox;
using ftxui::separator;
using ftxui::text;
using ftxui::vbox;

std::string format_currency_drill(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (val < 0) oss << "-$" << (-val);
    else         oss << "$" << val;
    return oss.str();
}

std::string format_pct_signed(double pct) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << (pct >= 0 ? "+" : "") << pct << "%";
    return oss.str();
}

std::string format_pct_unsigned(double pct) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << pct << "%";
    return oss.str();
}

std::string format_score_int(double score) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << score;
    return oss.str();
}

std::string pad_right(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// Replicate compute_shovel_score() from DashboardView.cpp.
// TODO(v0.3-3): hoist this into DiscoveryService alongside aggregate_
// supplier_spend() so the drill and the widget compute the headline
// identically without duplication.  For v0.3-2 the drill keeps a local
// copy to avoid coupling this task to a DashboardView header refactor.
double median_clamp_score(std::vector<double> velocities) {
    if (velocities.empty()) return 0.0;
    std::sort(velocities.begin(), velocities.end());
    const std::size_t n = std::min<std::size_t>(velocities.size(), 10);
    std::vector<double> top(velocities.end() - static_cast<long>(n),
                            velocities.end());
    std::sort(top.begin(), top.end());
    const double median = top[top.size() / 2];
    if (median < 0.0) return 0.0;
    if (median > 100.0) return 100.0;
    return median;
}

// Tier mapping mirrors ui_shovel_score.cpp's bands.  Returns the label
// + the FTXUI Color to render it in.
struct Tier {
    std::string  label;
    ftxui::Color color;
};
Tier tier_for(double score) {
    if (score >= 80) return { "AI POWERHOUSE",   ftxui::Color::Cyan };
    if (score >= 60) return { "EARLY ADOPTER",   ftxui::Color::Green };
    if (score >= 40) return { "BUILDING STACK",  ftxui::Color::Yellow };
    if (score >= 20) return { "GETTING STARTED", ftxui::Color::Yellow };
    return            { "WAITING TO DIG",   ftxui::Color::GrayLight };
}

Element render_breadcrumb() {
    return hbox({
        text("Dashboard") | dim,
        text(" > ") | color(kTokens.accent_info),
        text("Shovel Score") | bold | color(kTokens.fg_emphasized),
    });
}

}  // namespace

Drill_ShovelScore::Drill_ShovelScore(DataStore& data,
                                     std::string current_month,
                                     std::string previous_month)
    : data_(data),
      current_month_(std::move(current_month)),
      previous_month_(std::move(previous_month)) {}

ftxui::Element Drill_ShovelScore::render() const {
    // Step 1: aggregate per-supplier spend (the shared call site).
    const auto all_rows = DiscoveryService::instance().aggregate_supplier_spend(
        data_.transactions, current_month_, previous_month_);

    // Step 2: take the top-10 for the Inputs table — same slice the
    // formula uses internally so what the user sees on screen IS what
    // gets fed into the median.
    constexpr std::size_t kTopN = 10;
    std::vector<tf::services::SupplierSpend> inputs(
        all_rows.begin(),
        all_rows.begin() + std::min<std::size_t>(all_rows.size(), kTopN));

    // Step 3: build the |percent_change| vector and feed it to the
    // median-clamp formula.  When inputs is empty the score is 0.0
    // sentinel; we override the displayed score to "--" for that case.
    std::vector<double> velocities;
    velocities.reserve(inputs.size());
    for (const auto& r : inputs) velocities.push_back(std::abs(r.percent_change));

    const bool insufficient = inputs.empty();
    const double median = insufficient ? 0.0 : ([&]() {
        // We need both the raw median AND the clamped score below, so
        // we replicate the inner steps to surface the "Median |MoM|"
        // line in the formula trace.
        std::vector<double> sorted = velocities;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t n = std::min<std::size_t>(sorted.size(), 10);
        std::vector<double> top(sorted.end() - static_cast<long>(n),
                                sorted.end());
        std::sort(top.begin(), top.end());
        return top[top.size() / 2];
    })();
    const double clamped = median_clamp_score(velocities);

    // Step 4: build the element graph.
    Elements rows;
    rows.push_back(text(""));

    // Headline score + tier.
    if (insufficient) {
        rows.push_back(text("      --/100") | bold | color(kTokens.fg_dim));
        rows.push_back(text("      INSUFFICIENT DATA") | color(kTokens.fg_dim));
        rows.push_back(text(""));
        rows.push_back(text("  Link bank accounts to start scoring.") | dim);
    } else {
        const Tier t = tier_for(clamped);
        rows.push_back(text("      " + format_score_int(clamped) + "/100")
                       | bold | color(t.color));
        rows.push_back(text("      " + t.label) | color(t.color));
    }
    rows.push_back(text(""));

    // Formula card.
    rows.push_back(text("  Score formula (v0.2 stop-gap)") | bold);
    rows.push_back(hbox({
        text("  "),
        text(std::string(70, '-')) | dim,
    }));
    rows.push_back(text(
        "  median(|MoM velocity|) over top-10 suppliers by spend, clamped to [0,100]"
    ) | dim);
    rows.push_back(text(""));

    // Inputs table.
    rows.push_back(text("  Inputs") | bold);
    rows.push_back(hbox({
        text("  "),
        text(std::string(70, '-')) | dim,
    }));
    if (inputs.empty()) {
        rows.push_back(text("  No suppliers detected.") | dim);
    } else {
        // Column widths chosen so a 120-col canvas leaves margin both sides.
        for (const auto& r : inputs) {
            rows.push_back(hbox({
                text("  "),
                text(pad_right(r.ticker, 8)) | bold,
                text(pad_right(format_currency_drill(r.total_spend), 14)),
                text("cur ") | dim,
                text(pad_right(format_currency_drill(r.current_spend), 14)),
                text("prev ") | dim,
                text(pad_right(format_currency_drill(r.previous_spend), 14)),
                text(pad_right("MoM " + format_pct_signed(r.percent_change), 16)),
            }));
        }
    }
    rows.push_back(text(""));

    // Trace line (median + clamp arrow), only when we have data.
    if (!insufficient) {
        rows.push_back(text(
            "  Median |MoM|: " + format_pct_unsigned(median)
        ) | dim);
        rows.push_back(text(
            "  Clamped to [0,100]: " + format_score_int(clamped) + "  ->  "
                + format_score_int(clamped)
        ) | dim);
        rows.push_back(text(""));
    }

    // Stop-gap note.
    rows.push_back(text(
        "  Note: this is a stop-gap formula. v0.3+ will replace with a real scoring model."
    ) | dim);

    rows.push_back(filler());

    Element panel = vbox({
        hbox({ text("  "), render_breadcrumb() }),
        separator(),
        vbox(std::move(rows)),
        separator(),
        text("  [Esc] Back   [j/k] Scroll") | dim,
    }) | border;
    return panel | flex;
}

}  // namespace tf::views::drills
