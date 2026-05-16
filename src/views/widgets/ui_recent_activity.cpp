// ---------------------------------------------------------------------------
// ui_recent_activity.cpp — implementation of the "Recent Activity" panel.
// ---------------------------------------------------------------------------
// VISUAL
//   ╭ Recent Activity ─────────────────────────────╮
//   │ 2026-05-15  Zelle: Tristan Loftus    +$54.58 │
//   │ 2026-05-15  CITI AUTOPAY PAYMENT    -$500.00 │
//   │ ...                                          │
//   ╰──────────────────────────────────────────────╯
//
// COLOR DISCIPLINE
//   - Date column: dim.
//   - Description: default fg.
//   - Amount: green when >= 0, red when < 0.
//
// EMPTY STATE
//   "No transactions yet." dim, centered-ish under the title.

#include "ui_recent_activity.h"

#include "../ViewCommon.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {

std::string fmt(double amount) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (amount < 0) ss << "-$" << std::abs(amount);
    else            ss << "+$" << amount;
    return ss.str();
}

}  // namespace

Element RecentActivityRenderer(const std::vector<tf::widgets::RecentTx>& rows,
                               bool focused) {
    Element title = text("Recent Activity")
        | (focused ? (bold | color(kTokens.fg_emphasized))
                   : color(kTokens.fg_default));

    Elements body_rows;
    if (rows.empty()) {
        body_rows.push_back(text("  No transactions yet.") | color(kTokens.fg_dim));
    } else {
        for (const auto& r : rows) {
            body_rows.push_back(hbox({
                text("  " + r.date) | color(kTokens.fg_dim),
                text("  "),
                text(r.description) | flex,
                text("  "),
                text(fmt(r.amount)) |
                    color(r.amount >= 0 ? kTokens.accent_positive
                                        : kTokens.accent_negative),
                text("  "),
            }));
        }
    }

    Element panel = vbox({
        title,
        separator(),
        vbox(body_rows) | flex,
    });

    return panel | borderRounded
                 | (focused ? color(kTokens.focus) : color(kTokens.fg_dim));
}

}  // namespace ftxui
