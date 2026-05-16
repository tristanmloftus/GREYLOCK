// ---------------------------------------------------------------------------
// ui_cash_flow.cpp — implementation of "Cash Flow This Month".
// ---------------------------------------------------------------------------
// VISUAL
//   ╭ Cash Flow (this month) ────╮
//   │ Income     $X,XXX.XX       │
//   │ Expenses   $X,XXX.XX       │
//   │ Net        $X,XXX.XX       │   <- green if positive, red if negative
//   ╰────────────────────────────╯
//
// COLOR DISCIPLINE
//   - Income line value: kTokens.accent_positive (green).
//   - Expenses line value: kTokens.accent_negative (red).
//   - Net value: signed — green if >= 0, red if < 0.
//   - Title: bright bold when focused, dim otherwise.

#include "ui_cash_flow.h"

#include "../ViewCommon.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {

std::string fmt(double amount) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (amount < 0) ss << "-$" << std::abs(amount);
    else            ss << "$"  << amount;
    return ss.str();
}

}  // namespace

Element CashFlowThisMonthRenderer(double income,
                                  double expenses,
                                  double net,
                                  bool   focused) {
    Element title = text("Cash Flow (this month)")
        | (focused ? (bold | color(kTokens.fg_emphasized))
                   : color(kTokens.fg_default));

    auto row = [](const std::string& label, double value, Color value_color) {
        return hbox({
            text("  " + label) | color(kTokens.fg_dim),
            filler(),
            text(fmt(value)) | color(value_color),
            text("  "),
        });
    };

    Element body = vbox({
        row("Income",   income,   kTokens.accent_positive),
        row("Expenses", expenses, kTokens.accent_negative),
        separator(),
        row("Net",      net,      net >= 0 ? kTokens.accent_positive
                                           : kTokens.accent_negative),
    });

    Element panel = vbox({
        title,
        separator(),
        body | flex,
    });

    return panel | borderRounded
                 | (focused ? color(kTokens.focus) : color(kTokens.fg_dim));
}

}  // namespace ftxui
