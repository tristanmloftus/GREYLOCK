#pragma once

#include <ftxui/dom/elements.hpp>
#include "../models/DataStore.h"
#include "ViewCommon.h"
#include <algorithm>

class BudgetView {
public:
    explicit BudgetView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) { entity_id_ = id; }
    void set_selected(int index) { selected_ = index; }
    int get_selected() const { return selected_; }

    Element render(const std::string& current_month) {
        data_store_.calculate_spent_amounts(current_month);

        std::vector<Budget*> budgets;
        if (!entity_id_.empty()) {
            budgets = data_store_.get_budgets_for_entity_month(entity_id_, current_month);
        } else {
            budgets = data_store_.get_budgets_for_month(current_month);
        }

        Elements rows;
        rows.push_back(hbox({
            blue_dim("  Category"), text(" | "),
            blue_dim("Budget"), text(" | "),
            blue_dim("Spent"), text(" | "),
            blue_dim("Left"), text(" | "),
            blue_dim("Progress")
        }));

        int idx = 0;
        for (auto* b : budgets) {
            auto* cat = data_store_.get_category(b->category_id).value_or(nullptr);
            std::string cat_name = cat ? cat->name : b->category_id;

            double left = b->limit_amount - b->spent_amount;
            float ratio = b->limit_amount > 0 ? static_cast<float>(b->spent_amount / b->limit_amount) : 0.0f;
            auto gauge_elem = gauge(std::min(ratio, 1.0f)) | color(ratio > 1.0f ? Color::Red : Color::Green) | flex;

            auto row = hbox({
                text("  " + cat_name), text(" | "),
                text(format_currency(b->limit_amount)), text(" | "),
                text(format_currency(b->spent_amount)), text(" | "),
                text(format_currency(left)), text(" | "),
                gauge_elem
            });

            if (idx == selected_) {
                rows.push_back(row | bold | color(LED_BLUE));
            } else {
                rows.push_back(row | color(LED_BLUE_DIM));
            }
            idx++;
        }

        rows.push_back(text(""));
        rows.push_back(blue_dim("  [B] Add Budget  [Q] Quit"));

        return vbox(std::move(rows)) | flex;
    }

private:
    DataStore& data_store_;
    std::string entity_id_;
    int selected_ = 0;
};