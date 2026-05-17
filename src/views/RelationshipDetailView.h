#pragma once

// ---------------------------------------------------------------------------
// RelationshipDetailView — reference Panel 5.
// ---------------------------------------------------------------------------
//   $ open cade
//   cade hartford
//   ──────────────────────────────────────────────
//   role            operator · pcc · pending formalization
//   relationship    professional · 8 months
//   cadence         target 7d · actual 3d · ✓ on cadence
//                                                       | he owes you
//   working on      pcc integration layer · plaid       |  · ...
//                                                       | you owe him
//   last real       3 days ago · 2026-05-13             |  · ...
//   last text       yesterday
//                                                       | relevant decisions
//                                                       |  · ...
//                                                       | interaction freq · 90d
//                                                       | ▁▂▃▅▂▄▆▃▁▂▄▆▃▁▂▄▆▃▁

#include "DetailViewCommon.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace tf::views {

struct Relationship {
    std::string id;
    std::string display_name;
    std::string role_summary;       // "operator · pcc · pending formalization"
    std::string relationship_kind;  // "professional · 8 months"
    std::string cadence_summary;    // "target 7d · actual 3d · on cadence"
    bool        cadence_ok = true;

    std::string working_on;
    std::string last_real_conversation;   // "3 days ago · 2026-05-13"
    std::string last_text_exchange;       // "yesterday"

    std::vector<std::string> he_owes_you;
    std::vector<std::string> you_owe_him;
    std::vector<std::string> relevant_decisions;

    // 90-day interaction sparkline samples (one bar per ~3 days = 30 samples).
    // Values are roughly normalized 0..8; renderer maps to block glyphs.
    std::vector<int> interactions_90d;
};

class RelationshipDetailView {
public:
    explicit RelationshipDetailView() = default;

    void set_relationship(Relationship r) { r_ = std::move(r); has_data_ = true; }
    void clear()                          { r_ = Relationship{}; has_data_ = false; }
    bool has_data() const                 { return has_data_; }

    Element render() const {
        using namespace ftxui;
        using namespace tf::views::detail;

        if (!has_data_) {
            return vbox({
                text(""),
                text("  person · (no person opened)") | color(kTokens.fg_emphasized),
                text(""),
                text("  Use `:open <name>` to open a relationship.") | color(kTokens.fg_dim),
            }) | flex;
        }

        Element header = hbox({
            text(r_.display_name) | color(kTokens.fg_emphasized) | bold,
        });

        // Left column — biographical + activity rows.
        Element left = vbox({
            label_row("role",         r_.role_summary),
            label_row("relationship", r_.relationship_kind),
            hbox({
                text("  cadence") | color(kTokens.fg_dim) | size(WIDTH, EQUAL, 18),
                text(maybe(r_.cadence_summary))
                  | color(r_.cadence_ok ? kTokens.accent_positive
                                        : kTokens.accent_warning),
            }),
            text(""),
            label_row("working on",   r_.working_on),
            text(""),
            label_row("last real",    r_.last_real_conversation),
            label_row("last text",    r_.last_text_exchange),
        });

        // Right column — reciprocity + decisions + sparkline.
        Elements right_rows;
        right_rows.push_back(heading("he owes you")  | color(kTokens.accent_negative));
        right_rows.push_back(bulleted_list(r_.he_owes_you, "nothing outstanding"));
        right_rows.push_back(text(""));
        right_rows.push_back(heading("you owe him")  | color(kTokens.accent_warning));
        right_rows.push_back(bulleted_list(r_.you_owe_him, "nothing outstanding"));
        right_rows.push_back(text(""));
        right_rows.push_back(heading("relevant decisions"));
        right_rows.push_back(bulleted_list(r_.relevant_decisions, "—"));
        right_rows.push_back(text(""));
        right_rows.push_back(heading("interaction frequency · last 90 days"));
        right_rows.push_back(text("  " + sparkline(r_.interactions_90d))
                             | color(kTokens.thesis_up));

        Element right = vbox(std::move(right_rows));

        return vbox({
            text(""),
            header,
            separator() | color(kTokens.fg_dim),
            hbox({
                left  | flex,
                separator() | color(kTokens.fg_dim),
                right | flex,
            }) | flex,
        }) | flex;
    }

private:
    Relationship r_{};
    bool         has_data_ = false;

    // Render a vector of small ints as Unicode block bars.  Empty input
    // returns a single em-dash so callers don't special-case the empty
    // path.
    static std::string sparkline(const std::vector<int>& samples) {
        static const char* glyphs[] = {
            " ", "▁", "▂", "▃", "▄",
            "▅", "▆", "▇", "█"
        };
        if (samples.empty()) return "—";
        std::string out;
        for (int v : samples) {
            int clamped = v < 0 ? 0 : (v > 8 ? 8 : v);
            out += glyphs[clamped];
        }
        return out;
    }
};

}  // namespace tf::views
