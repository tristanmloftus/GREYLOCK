#pragma once

#include <ftxui/dom/elements.hpp>
#include "../models/DataStore.h"
#include "ViewCommon.h"

class AccountsView {
public:
    explicit AccountsView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) { entity_id_ = id; }
    void set_selected(int index) { selected_ = index; }
    int get_selected() const { return selected_; }

    Element render() {
        std::vector<Account*> filtered_accounts;
        if (!entity_id_.empty()) {
            filtered_accounts = data_store_.get_accounts_for_entity(entity_id_);
        } else {
            for (auto& acc : data_store_.accounts) {
                filtered_accounts.push_back(&acc);
            }
        }

        Elements rows;
        rows.push_back(hbox({
            blue_dim("  #"), text(" | "),
            blue_dim("Name"), text(" | "),
            blue_dim("Type"), text(" | "),
            blue_dim("Balance")
        }));

        int idx = 0;
        double total_balance = 0.0;
        for (auto* acc : filtered_accounts) {
            std::string type_str;
            switch (acc->type) {
                case AccountType::Checking: type_str = "Checking"; break;
                case AccountType::Savings: type_str = "Savings"; break;
                case AccountType::CreditCard: type_str = "Credit"; break;
                case AccountType::Investment: type_str = "Invest"; break;
                case AccountType::Cash: type_str = "Cash"; break;
                default: type_str = "Other"; break;
            }

            auto row = hbox({
                text("  " + std::to_string(idx + 1)), text(" | "),
                text(acc->name), text(" | "),
                text(type_str), text(" | "),
                DecorateAmount(acc->balance)
            });

            if (idx == selected_) {
                rows.push_back(row | bold | color(LED_BLUE));
            } else {
                rows.push_back(row | color(LED_BLUE_DIM));
            }
            total_balance += acc->balance;
            idx++;
        }

        if (filtered_accounts.empty()) {
            rows.push_back(text("  No accounts. Press [L] to link via Plaid.") | dim);
        }

        if (!filtered_accounts.empty()) {
            rows.push_back(separator());
            rows.push_back(hbox({
                blue_bold("  Total Balance:  "),
                DecorateAmount(total_balance) | bold
            }));
        }

        rows.push_back(text(""));
        rows.push_back(text("  [L] Link Plaid  [S] Save  [Q] Quit") | dim);

        return vbox(std::move(rows)) | flex;
    }

private:
    DataStore& data_store_;
    std::string entity_id_;
    int selected_ = 0;
};