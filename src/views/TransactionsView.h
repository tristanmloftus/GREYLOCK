#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include "../models/DataStore.h"
#include "ViewCommon.h"
#include <algorithm>

class TransactionsView {
public:
    explicit TransactionsView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) {
        entity_id_ = id;
        auto accounts = data_store_.get_accounts_for_entity(entity_id_);
        if (!accounts.empty()) {
            account_id_ = accounts[0]->id;
        }
    }
    void set_selected(int index) { selected_ = index; }
    int get_selected() const { return selected_; }
    void set_account_id(const std::string& id) { account_id_ = id; }

    Element render() {
        auto tx_list = data_store_.get_transactions_for_account(account_id_);

        auto term_width = Terminal::Size().dimx;
        int desc_width = std::max(20, term_width - 45);

        Elements rows;
        rows.push_back(hbox({
            blue_dim("  #"), text(" | "),
            blue_dim("Date"), text(" | "),
            blue_dim("Description"), text(" | "),
            blue_dim("Amount")
        }));

        int idx = 0;
        for (auto* tx : tx_list) {
            std::string desc = tx->description.substr(0, desc_width);
            if (tx->description.length() > static_cast<size_t>(desc_width))
                desc += "...";

            auto row = hbox({
                text("  " + std::to_string(idx + 1)), text(" | "),
                text(tx->date), text(" | "),
                text(desc), text(" | "),
                DecorateAmount(tx->amount)
            });

            if (idx == selected_) {
                rows.push_back(row | bold | color(LED_BLUE));
            } else {
                rows.push_back(row | color(LED_BLUE_DIM));
            }
            idx++;
        }

        if (tx_list.empty()) {
            rows.push_back(text("  No transactions - link account via Plaid to sync.") | dim);
        }

        rows.push_back(text(""));
        rows.push_back(text("  [P] Link Plaid Account  [L] Sync Latest  [Q] Quit") | dim);

        return vbox(std::move(rows)) | flex;
    }

private:
    DataStore& data_store_;
    std::string entity_id_;
    int selected_ = 0;
    std::string account_id_;
};