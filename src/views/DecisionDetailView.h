#pragma once

// ---------------------------------------------------------------------------
// DecisionDetailView — reference Panel 4.
// ---------------------------------------------------------------------------
//   $ open decision services-arm
//   decision · build the services arm slowly · 2024-04-17
//   ─────────────────────────────────────────────────────
//   deciders        rory loftus              | touches    #pcc tristan ...
//   status          confirmed                | source     vault: ...md
//   entity scope    #pcc                     | commit     a3f9c12
//   confidence      0.7                      | outcome    not yet logged
//                                            |            prompt in 47 days
//   rationale
//     we decided to build ...
//
//   alternatives considered
//     · build it fast and borrow
//     · partner it out
//     · don't build it at all
//
// Drives off a single Decision POD; empty fields render as "—".  No
// backend wiring yet — App holds a current_decision_ that's populated
// when `:open decision <id>` lands.

#include "DetailViewCommon.h"

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace tf::views {

struct Decision {
    std::string id;
    std::string title;
    std::string body_rationale;
    std::vector<std::string> alternatives;

    std::string deciders;          // pre-formatted "rory loftus" etc.
    std::string status;            // open|confirmed|rolled_back|...
    std::string entity_scope;      // "#pcc" / "#me-rory" ...
    std::string confidence_str;    // pre-formatted "0.7" or ""
    std::vector<std::string> touches;

    std::string source_vault_path;
    std::string source_commit_sha;

    std::string outcome_status;    // "not yet logged" | ...
    std::string outcome_prompt;    // "prompt in 47 days"

    std::string decided_at_date;   // "2024-04-17" or ""
};

class DecisionDetailView {
public:
    explicit DecisionDetailView() = default;

    void set_decision(Decision d) { d_ = std::move(d); has_data_ = true; }
    void clear()                  { d_ = Decision{}; has_data_ = false; }
    bool has_data() const         { return has_data_; }

    // Tile mode for HomeView's panel 4.  Drops the two-column field
    // layout in favor of a 4-row meta block + 2-line rationale + a
    // compressed alternatives bullet list.  Reuses DetailViewCommon
    // builders so style stays single-sourced.
    Element render_tile() const {
        using namespace ftxui;
        using namespace tf::views::detail;
        if (!has_data_) {
            return vbox({
                text(""),
                text("  decision · (no recent decision)") | color(kTokens.fg_emphasized),
                text(""),
                text("  Log one via vault → ingest, or `:open decision <id>`.")
                  | color(kTokens.fg_dim),
            }) | flex;
        }
        Element header = header_strip("decision", d_.title,
                                       d_.decided_at_date.empty() ? "—" : d_.decided_at_date);
        Element meta = vbox({
            label_row("status",       d_.status, 14),
            label_row("entity scope", d_.entity_scope, 14),
            label_row("deciders",     d_.deciders, 14),
            label_row("outcome",      d_.outcome_status, 14),
        });
        return vbox({
            header,
            separator() | color(kTokens.fg_dim),
            meta,
            text(""),
            heading("rationale"),
            body_paragraph(d_.body_rationale),
            text(""),
            heading("alternatives"),
            bulleted_list(d_.alternatives, "—"),
            filler(),
        }) | flex;
    }

    Element render() const {
        using namespace ftxui;
        using namespace tf::views::detail;

        if (!has_data_) {
            return vbox({
                text(""),
                text("  decision · (no decision opened)") | color(kTokens.fg_emphasized),
                text(""),
                text("  Use `:open decision <id>` to open one.") | color(kTokens.fg_dim),
            }) | flex;
        }

        Element header = header_strip("decision", d_.title,
                                       d_.decided_at_date.empty() ? "—" : d_.decided_at_date);

        Element left_fields = vbox({
            label_row("deciders",      d_.deciders),
            label_row("status",        d_.status),
            label_row("entity scope",  d_.entity_scope),
            label_row("confidence",    d_.confidence_str),
        });

        Element right_fields = vbox({
            hbox({ text("  touches") | color(kTokens.fg_dim) | size(WIDTH, EQUAL, 12),
                   tag_chips(d_.touches) }),
            label_row("source",  d_.source_vault_path, 12),
            label_row("commit",  d_.source_commit_sha, 12),
            label_row("outcome", d_.outcome_status,    12),
            d_.outcome_prompt.empty()
                ? text("")
                : hbox({ text("  ")           | size(WIDTH, EQUAL, 12),
                         text(d_.outcome_prompt) | color(kTokens.accent_warning) }),
        });

        return vbox({
            text(""),
            header,
            separator() | color(kTokens.fg_dim),
            hbox({
                left_fields  | flex,
                separator()  | color(kTokens.fg_dim),
                right_fields | flex,
            }),
            text(""),
            heading("rationale"),
            body_paragraph(d_.body_rationale),
            text(""),
            heading("alternatives considered"),
            bulleted_list(d_.alternatives, "no alternatives recorded"),
        }) | flex;
    }

private:
    Decision d_{};
    bool     has_data_ = false;
};

}  // namespace tf::views
