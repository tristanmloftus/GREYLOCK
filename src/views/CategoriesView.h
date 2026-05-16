#pragma once

// ---------------------------------------------------------------------------
// CategoriesView — flat list of categories scoped to the current entity.
// ---------------------------------------------------------------------------
// Read-only foundation for v2 (greylock-spec.md §8.6).  Two-level hierarchy,
// modal picker, auto-categorization rules sub-view are all deferred — this
// surface exists so `g c` has somewhere to land and so users can see the
// raw category list as it grows.

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

#include "../models/DataStore.h"
#include "ViewCommon.h"

class CategoriesView {
public:
    explicit CategoriesView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) { entity_id_ = id; }
    void set_selected(int index) { selected_ = index; }
    int  get_selected() const    { return selected_; }

    Element render() {
        // Categories are global on the TUI side today (no entity_id on
        // the Category model). The server-side `categories` table is
        // entity-scoped; surfacing that distinction requires propagating
        // entity_id through RemoteBackendStorageService. Deferred until
        // a per-entity category UX is actually needed.
        std::vector<const Category*> filtered;
        for (const auto& cat : data_store_.categories) {
            filtered.push_back(&cat);
        }

        Elements rows;
        rows.push_back(hbox({
            blue_dim("  #"),    text(" | "),
            blue_dim("Name"),   text(" | "),
            blue_dim("Type"),
        }));

        int idx = 0;
        for (const auto* cat : filtered) {
            std::string type_str;
            switch (cat->type) {
                case CategoryType::Income:   type_str = "Income";   break;
                case CategoryType::Transfer: type_str = "Transfer"; break;
                default:                     type_str = "Expense";  break;
            }
            auto row = hbox({
                text("  " + std::to_string(idx + 1)), text(" | "),
                text(cat->name),                       text(" | "),
                text(type_str),
            });
            if (idx == selected_) rows.push_back(row | bold | color(LED_BLUE));
            else                  rows.push_back(row | color(LED_BLUE_DIM));
            idx++;
        }

        if (filtered.empty()) {
            rows.push_back(text("  No categories yet.") | dim);
            rows.push_back(text("  v2: add/edit/auto-rules come from the backend.")
                           | color(kTokens.fg_dim));
        }

        rows.push_back(text(""));
        rows.push_back(text("  [g t] back to Transactions  [Q] Quit") | dim);
        return vbox(std::move(rows)) | flex;
    }

private:
    DataStore&  data_store_;
    std::string entity_id_;
    int         selected_ = 0;
};
