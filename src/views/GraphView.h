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

    int  depth() const { return depth_; }

    Element render() const {
        using namespace ftxui;
        Elements rows;
        int node_count = 1;   // root
        int edge_count = 0;

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

            if (depth_ < 2) continue;

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

        // Knows / Decisions / Relationships counts (v3+).  These read
        // from the server-side tables once GET /graph lands; until then,
        // the entries appear as "0 logged" which still matches the
        // reference structure.
        rows.push_back(text("  ├── knows · 0 people indexed") | color(kTokens.fg_dim));
        rows.push_back(text("  ╰── decisions · 0 logged")     | color(kTokens.fg_dim));

        // Footer.
        Element footer = hbox({
            text("  depth " + std::to_string(depth_) + " · " +
                 std::to_string(node_count) + " nodes · " +
                 std::to_string(edge_count) + " edges · "
                 "filter: f · expand: enter")
              | color(kTokens.fg_dim),
        });

        return vbox({
            text(""),
            vbox(std::move(rows)),
            filler(),
            separator() | color(kTokens.fg_dim),
            footer,
        }) | flex;
    }

private:
    DataStore&  data_store_;
    int         depth_ = 2;
    std::string principal_ = "rory loftus";

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
