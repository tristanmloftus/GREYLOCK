#pragma once

// ---------------------------------------------------------------------------
// DetailViewCommon — chrome + field-row helpers shared by every
// `open <type> <id>` detail view (reference Panels 4 + 5).
// ---------------------------------------------------------------------------
// Visual contract:
//   <subtitle> · <title> · <stamp>
//   ──────────────────────────────────────────────
//   <field>     <value>
//   <field>     <value>
//   ...
//   <heading>
//     <body / bullets>
//   ──────────────────────────────────────────────
//   <right-rail block 1>
//   <right-rail block 2>
//
// Empty values render as a single em-dash via maybe(). Tag chips
// render as space-separated colored tokens.

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

#include "ViewCommon.h"

namespace tf::views::detail {

inline std::string maybe(const std::string& s) {
    return s.empty() ? std::string("—") : s;
}

inline Element header_strip(const std::string& subtitle,
                            const std::string& title,
                            const std::string& stamp) {
    return hbox({
        text(subtitle) | color(kTokens.fg_dim),
        text(" · ")    | color(kTokens.fg_dim),
        text(title)    | color(kTokens.fg_emphasized) | bold,
        text(" · ")    | color(kTokens.fg_dim),
        text(stamp)    | color(kTokens.fg_dim),
    });
}

inline Element label_row(const std::string& label,
                         const std::string& value,
                         int label_width = 18) {
    return hbox({
        text("  " + label) | color(kTokens.fg_dim)
                          | size(WIDTH, EQUAL, label_width),
        text(maybe(value)) | color(kTokens.fg_default),
    });
}

inline Element heading(const std::string& title) {
    return text("  " + title) | color(kTokens.fg_emphasized) | bold;
}

inline Element body_paragraph(const std::string& md) {
    return paragraph(md.empty() ? std::string("—") : md)
         | color(kTokens.fg_default);
}

inline Element bulleted_list(const std::vector<std::string>& items,
                             const std::string& empty_message = "—") {
    if (items.empty()) {
        return text("  " + empty_message) | color(kTokens.fg_dim);
    }
    Elements rows;
    for (const auto& it : items) {
        rows.push_back(text("  · " + it) | color(kTokens.fg_default));
    }
    return vbox(std::move(rows));
}

inline Element tag_chips(const std::vector<std::string>& tags) {
    if (tags.empty()) return text("—") | color(kTokens.fg_dim);
    Elements parts;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        parts.push_back(text(tags[i]) | color(kTokens.accent_positive));
        if (i + 1 < tags.size()) {
            parts.push_back(text("  ") | color(kTokens.fg_dim));
        }
    }
    return hbox(std::move(parts));
}

}  // namespace tf::views::detail
