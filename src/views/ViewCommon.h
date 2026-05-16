#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace ftxui;

const Color LED_BLUE = Color(39, 170, 255);
const Color LED_BLUE_DIM = Color(20, 85, 135);

// ---------------------------------------------------------------------------
// ColorTokens — semantic palette introduced for the v0.3 UX redesign.
// ---------------------------------------------------------------------------
// Every color in the dashboard maps to exactly one of these tokens.  The
// `focus` token is the strongest UI signal — yellow border + bright title
// on the currently-focused widget — and is reserved for that purpose
// (no widget data uses yellow).  See docs/UI_REDESIGN_V0.3.md §3e for the
// full token rationale and §3a for the focus model that consumes them.
//
// Task v0.3-1 introduced this struct (focus / fg_* / accent_* tokens).
// Task v0.3-5 added `thesis_up` — the magenta accent reserved for
// "value going up is *interesting*" widgets (ui_category_trends) —
// and migrated every widget off raw `Color::*` literals onto these tokens.
//
// Semantic discipline: each token has ONE meaning.  `accent_warning`
// and `focus` happen to share the same FTXUI Color (Yellow) but carry
// distinct semantics: warning = "needs attention" on a data row;
// focus = "the cursor is here" on a chrome element.  No data row uses
// the `focus` token; no chrome element uses `accent_warning`.
// ---------------------------------------------------------------------------
struct ColorTokens {
    Color focus;              // Focused-widget border + selection arrow.
    Color fg_default;         // Body text.
    Color fg_dim;             // Secondary labels, breadcrumbs, flat-trend rows.
    Color fg_emphasized;      // Headline values, focused-widget title.
    Color accent_positive;    // Value >= 0 / surplus / spending decreased.
    Color accent_negative;    // Value <  0 / error state / disconnected.
    Color accent_warning;     // Needs attention (stale sync, over-budget).
    Color accent_info;        // Neutral metadata (timestamps, top-tier label).
    Color thesis_up;          // Thesis accent: this number going up is *interesting*.
};

// Default theme.  Initialised inline so headers that include ViewCommon.h
// pick up the palette without a separate .cpp.  See §3e of the redesign
// proposal for the binding of each token to its FTXUI Color value.
inline const ColorTokens kTokens = {
    /*focus=*/           Color::Yellow,
    /*fg_default=*/      Color::Default,
    /*fg_dim=*/          Color::GrayDark,
    /*fg_emphasized=*/   Color::White,
    /*accent_positive=*/ Color::Green,
    /*accent_negative=*/ Color::Red,
    /*accent_warning=*/  Color::Yellow,
    /*accent_info=*/     Color::Cyan,
    /*thesis_up=*/       Color::Magenta,
};

// ---------------------------------------------------------------------------
// KeyHint — one row in the contextual status-bar footer.
// ---------------------------------------------------------------------------
// Each widget exposes a `hints_when_focused()` free function returning a
// vector<KeyHint>.  The StatusBar (owned by v0.3-4) joins these with the
// global hint set into the two-row footer.  The same registry powers the
// `?` help overlay so the two surfaces cannot drift apart.
//
// `key` is the visible label ("Enter", "j/k", "Esc") — verbatim as it
// should appear in the bar.  `label` is the human description.
// ---------------------------------------------------------------------------
struct KeyHint {
    std::string key;
    std::string label;
};

inline std::string format_currency(double amount) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (amount < 0) {
        ss << "-$" << std::abs(amount);
    } else {
        ss << "$" << amount;
    }
    return ss.str();
}

inline Element DecorateAmount(double val) {
    return text(format_currency(val)) | color(val >= 0 ? Color::Green : Color::Red) | bold;
}

inline Element blue_text(const std::string& s) {
    return text(s) | color(LED_BLUE);
}

inline Element blue_dim(const std::string& s) {
    return text(s) | color(LED_BLUE_DIM);
}

inline Element blue_bold(const std::string& s) {
    return text(s) | bold | color(LED_BLUE);
}