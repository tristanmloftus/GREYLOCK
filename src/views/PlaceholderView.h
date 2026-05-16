#pragma once

// ---------------------------------------------------------------------------
// PlaceholderView — generic "this surface is scaffolded but not yet
// populated" panel.  Used by the v3 / v4 view skeletons that the spec
// reserves keymap real estate for (greylock-spec.md §8.12–§8.18) before
// the data flow exists end-to-end.
//
// Each instance carries a title + a brief sentence explaining the
// roadmap stage. The view is keyboard-inert; nav happens via `g`+letter
// from the App.
// ---------------------------------------------------------------------------

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

#include "ViewCommon.h"

class PlaceholderView {
public:
    PlaceholderView(std::string title, std::string subtitle, std::string status)
        : title_(std::move(title)),
          subtitle_(std::move(subtitle)),
          status_(std::move(status)) {}

    Element render() const {
        std::vector<Element> rows;
        rows.push_back(text(""));
        rows.push_back(text("  " + title_) | bold | color(LED_BLUE));
        rows.push_back(text(""));
        rows.push_back(text("  " + subtitle_) | color(kTokens.fg_default));
        rows.push_back(text(""));
        rows.push_back(text("  " + status_) | color(kTokens.fg_dim));
        rows.push_back(text(""));
        rows.push_back(text("  Press ? for the keymap, : for the palette.") | dim);
        return vbox(std::move(rows)) | flex;
    }

private:
    std::string title_;
    std::string subtitle_;
    std::string status_;
};
