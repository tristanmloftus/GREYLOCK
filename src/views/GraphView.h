#pragma once

// ---------------------------------------------------------------------------
// GraphView — typed-knowledge-graph tree renderer (reference Panel 2).
// ---------------------------------------------------------------------------
// Renders an ASCII tree rooted at "you · <principal name>" with edges
// labeled by typed verbs (owns, employs, holds, targets, knows, decisions).
// Today the graph is computed entirely client-side from DataStore (which
// holds the v1 banking objects) plus simple count summaries for the v3/v4
// surfaces (decisions, targets, relationships, notes).  When the server-
// side edges table starts getting populated by ingestion, the same view
// will read from an authoritative GET /graph endpoint without changing
// its visual shape.
//
// Layout matches the reference panel:
//   you · <principal name>
//   ├── owns · <entity> · <kind>
//   │   ├── employs · ...
//   │   ├── holds · N accounts · <names>
//   │   ├── targets · N in pipeline
//   │   ╰── + N more
//   ├── owns · <real estate vehicle> · N properties
//   ├── knows · <person> · <role>
//   ╰── decisions · N logged
//
// Footer:
//   depth N · X nodes · Y edges · filter: f · expand: enter

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "../models/DataStore.h"
#include "ViewCommon.h"

class GraphView {
public:
    explicit GraphView(DataStore& data_store) : data_store_(data_store) {}

    void set_depth(int d)                          { depth_ = d < 1 ? 1 : (d > 5 ? 5 : d); }
    void set_principal_name(const std::string& s)  { principal_ = s; }

    // Counts fetched from the v3/v4 endpoints by the App on `:graph`.
    void set_decision_count(int n)                 { decision_count_ = n; }
    void set_relationship_count(int n)             { relationship_count_ = n; }
    void set_target_count(int n)                   { target_count_ = n; }
    void set_target_names(std::vector<std::string> v) { target_names_ = std::move(v); }
    void set_relationship_names(std::vector<std::string> v) { relationship_names_ = std::move(v); }
    void set_decision_titles(std::vector<std::string> v)    { decision_titles_ = std::move(v); }

    int  depth() const { return depth_; }

    // Tile mode for HomeView's 5-panel grid (reference Panel 2).  Depth
    // is pinned to 2; footer hint condensed to one line; no chrome
    // interaction (`f` / `enter` hints stripped since the tile is read-
    // only — the user navigates via `g G` to get the full-pane GraphView).
    Element render_tile() const {
        using namespace ftxui;
        int nodes = 0, edges = 0;
        Elements rows = build_tree_rows(/*pinned_depth=*/2, nodes, edges);
        // Condensed footer: command-only, no count chrome.
        Element footer = text("  graph --depth 2 · "
                              + std::to_string(nodes) + " nodes · "
                              + std::to_string(edges) + " edges")
                       | color(kTokens.fg_dim);
        return vbox({
            rows.empty() ? text("") : vbox(std::move(rows)),
            filler(),
            footer,
        }) | flex;
    }

    Element render() const {
        using namespace ftxui;
        int nodes = 0, edges = 0;
        Elements rows = build_tree_rows(depth_, nodes, edges);
        Element footer = hbox({
            text("  depth " + std::to_string(depth_) + " · " +
                 std::to_string(nodes) + " nodes · " +
                 std::to_string(edges) + " edges · "
                 "filter: f · expand: enter")
              | color(kTokens.fg_dim),
        });
        return vbox({
            text(""),
            rows.empty() ? text("") : vbox(std::move(rows)),
            filler(),
            separator() | color(kTokens.fg_dim),
            footer,
        }) | flex;
    }

private:
    // Build the typed-knowledge-graph tree rows.  Shared between
    // render() (full-pane) and render_tile() (HomeView grid).  Mutates
    // node_count / edge_count for footer aggregation.
    Elements build_tree_rows(int eff_depth,
                             int& node_count,
                             int& edge_count) const {
        using namespace ftxui;
        Elements rows;
        node_count = 1;
        edge_count = 0;

        rows.push_back(text("  you · " + principal_) | color(kTokens.fg_emphasized));

        // Top-level branches.  Order matches the reference: owned
        // entities first, then knowns, then decisions count.
        const auto& entities = data_store_.entities;
        for (std::size_t ei = 0; ei < entities.size(); ++ei) {
            const auto& e = entities[ei];
            const bool last_entity =
                (ei + 1 == entities.size()) /* and no knows/decisions follow */
                && false; // we always render knowns/decisions afterwards
            const std::string elbow = "├── ";
            // Render: ├── owns · <name> · <kind>
            rows.push_back(text(std::string("  ") + elbow +
                                "owns · " + e.name + " · " + entity_kind_label(e))
                           | color(kTokens.fg_default));
            ++node_count; ++edge_count;
            (void)last_entity;

            if (eff_depth < 2) continue;

            // Accounts under this entity.
            auto accounts = data_store_.get_accounts_for_entity(e.id);
            if (!accounts.empty()) {
                std::string acct_names;
                int linked = 0;
                for (std::size_t i = 0; i < accounts.size() && i < 4; ++i) {
                    if (!acct_names.empty()) acct_names += ", ";
                    acct_names += accounts[i]->institution.empty()
                                  ? accounts[i]->name
                                  : accounts[i]->institution;
                    if (accounts[i]->is_plaid_linked) ++linked;
                }
                if (accounts.size() > 4) {
                    acct_names += ", +" + std::to_string(accounts.size() - 4) + " more";
                }
                rows.push_back(text("  │   ├── holds · " +
                                    std::to_string(accounts.size()) + " account" +
                                    (accounts.size() == 1 ? "" : "s") + " · " + acct_names)
                               | color(kTokens.fg_default));
                node_count += static_cast<int>(accounts.size());
                edge_count += static_cast<int>(accounts.size());
            }

            // Recent transactions count under this entity (proxy until
            // real "targets"/etc data lands).
            int tx_for_entity = 0;
            for (const auto& tx : data_store_.transactions) {
                for (const auto* a : accounts) {
                    if (tx.account_id == a->id) { ++tx_for_entity; break; }
                }
            }
            if (tx_for_entity > 0) {
                rows.push_back(text("  │   ╰── " + std::to_string(tx_for_entity) +
                                    " transaction" + (tx_for_entity == 1 ? "" : "s") +
                                    " indexed")
                               | color(kTokens.fg_dim));
            }
        }

        // Knows / Targets / Decisions counts (v3+) — populated by the
        // App calling set_*_count after fetching the v3/v4 GET endpoints.
        if (target_count_ > 0) {
            rows.push_back(text("  ├── targets · " + std::to_string(target_count_)
                                + " in pipeline") | color(kTokens.fg_default));
            // Show up to depth_ target names indented.
            int shown = 0;
            const int max_show = std::max(0, std::min<int>((int)target_names_.size(), eff_depth + 1));
            for (; shown < max_show; ++shown) {
                const bool last = (shown + 1 == max_show);
                std::string elbow = last ? "╰── " : "├── ";
                rows.push_back(text("  │   " + elbow + target_names_[shown])
                               | color(kTokens.fg_default));
                ++node_count; ++edge_count;
            }
            if ((int)target_names_.size() > max_show) {
                rows.push_back(text("  │       + " +
                                    std::to_string((int)target_names_.size() - max_show)
                                    + " more") | color(kTokens.fg_dim));
            }
        }
        if (relationship_count_ > 0) {
            rows.push_back(text("  ├── knows · " + std::to_string(relationship_count_)
                                + " " + (relationship_count_ == 1 ? "person" : "people")
                                + " indexed") | color(kTokens.fg_default));
            int shown = 0;
            const int max_show = std::max(0, std::min<int>((int)relationship_names_.size(), eff_depth + 1));
            for (; shown < max_show; ++shown) {
                const bool last = (shown + 1 == max_show);
                std::string elbow = last ? "╰── " : "├── ";
                rows.push_back(text("  │   " + elbow + relationship_names_[shown])
                               | color(kTokens.fg_default));
                ++node_count; ++edge_count;
            }
        } else {
            rows.push_back(text("  ├── knows · 0 people indexed") | color(kTokens.fg_dim));
        }
        if (decision_count_ > 0) {
            rows.push_back(text("  ╰── decisions · " + std::to_string(decision_count_)
                                + " logged") | color(kTokens.fg_default));
            int shown = 0;
            const int max_show = std::max(0, std::min<int>((int)decision_titles_.size(), eff_depth + 1));
            for (; shown < max_show; ++shown) {
                const bool last = (shown + 1 == max_show);
                std::string elbow = last ? "╰── " : "├── ";
                rows.push_back(text("      " + elbow + decision_titles_[shown])
                               | color(kTokens.fg_default));
                ++node_count;
            }
        } else {
            rows.push_back(text("  ╰── decisions · 0 logged") | color(kTokens.fg_dim));
        }
        node_count += target_count_ + relationship_count_ + decision_count_;
        return rows;
    }

    DataStore&  data_store_;
    int         depth_ = 2;
    std::string principal_ = "rory loftus";

    int decision_count_     = 0;
    int relationship_count_ = 0;
    int target_count_       = 0;
    std::vector<std::string> target_names_;
    std::vector<std::string> relationship_names_;
    std::vector<std::string> decision_titles_;

    static std::string entity_kind_label(const Entity& e) {
        switch (e.type) {
            case EntityType::LLC:        return "llc";
            case EntityType::Corporation:return "corporation";
            case EntityType::Trust:      return "trust";
            case EntityType::Individual: return "individual";
            default:                     return "entity";
        }
    }
};
