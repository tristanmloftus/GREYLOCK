#pragma once

#include <ftxui/dom/elements.hpp>
#include <sstream>
#include <iomanip>
#include <map>

#include "../models/DataStore.h"
#include "../models/Account.h"
#include "../services/DiscoveryService.h"
#include "../services/ConsolidationService.h"
#include "ViewCommon.h"

struct SupplierSpend {
    std::string ticker;
    double amount;
    double percent_change;
};

struct CategoryTrend {
    std::string category_name;
    std::string emoji;
    double current_spend;
    double percent_change;
};

class DashboardView {
public:
    explicit DashboardView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) { entity_id_ = id; }

    Element render(const std::string& current_month) {
        double net_worth = 0;
        double checking = 0;
        double savings = 0;
        double credit = 0;
        double investment = 0;
        double income = 0;
        double spending = 0;

        auto& discovery = DiscoveryService::instance();

        if (!entity_id_.empty()) {
            net_worth = data_store_.get_total_net_worth_for_entity(entity_id_);
            auto accounts = data_store_.get_accounts_for_entity(entity_id_);
            for (auto* acc : accounts) {
                if (acc->type == AccountType::Checking) checking += acc->balance;
                else if (acc->type == AccountType::Savings) savings += acc->balance;
                else if (acc->type == AccountType::CreditCard) credit += acc->balance;
                else if (acc->type == AccountType::Investment) investment += acc->balance;
            }
            auto transactions = data_store_.get_transactions_for_entity(entity_id_);
            for (auto* tx : transactions) {
                if (tx->date.substr(0, 7) == current_month) {
                    if (tx->is_income()) income += tx->amount;
                    else spending += std::abs(tx->amount);
                }
            }
        } else {
            net_worth = data_store_.get_total_net_worth();
            checking = data_store_.get_total_balance_by_type(AccountType::Checking);
            savings = data_store_.get_total_balance_by_type(AccountType::Savings);
            credit = data_store_.get_total_balance_by_type(AccountType::CreditCard);
            investment = data_store_.get_total_balance_by_type(AccountType::Investment);
            income = data_store_.get_income_for_month(current_month);
            spending = data_store_.get_spending_for_month(current_month);
        }

        std::vector<SupplierSpend> shovel_suppliers;
        std::map<std::string, double> supplier_totals;
        double total_shovel_spend = 0;

        for (const auto& tx : data_store_.transactions) {
            auto ticker = discovery.mapToSupplier(tx.description);
            if (ticker) {
                supplier_totals[*ticker] += std::abs(tx.amount);
                if (tx.amount < 0) total_shovel_spend += std::abs(tx.amount);
            }
        }

        for (const auto& [ticker, amount] : supplier_totals) {
            SupplierSpend ss;
            ss.ticker = ticker;
            ss.amount = amount;
            ss.percent_change = 0;
            shovel_suppliers.push_back(ss);
        }

        double shovel_score = 0;
        if (!shovel_suppliers.empty()) {
            shovel_score = std::min(100.0, shovel_suppliers.size() * 20.0 + (total_shovel_spend / 1000.0) * 10.0);
        }

        std::vector<CategoryTrend> category_trends;
        std::vector<std::pair<std::string, std::string>> cat_info = {
            {"Food & Dining", "🍔"},
            {"Transportation", "🚗"},
            {"Shopping", "🛍️"},
            {"Entertainment", "🎬"},
            {"Utilities", "💡"}
        };
        std::vector<std::string> cat_ids = {"cat_food", "cat_transport", "cat_shopping", "cat_entertainment", "cat_utilities"};

        for (size_t i = 0; i < cat_info.size(); ++i) {
            double spend = 0;
            for (const auto& tx : data_store_.transactions) {
                if (tx.category_id == cat_ids[i] && tx.date.substr(0, 7) == current_month && tx.amount < 0) {
                    spend += std::abs(tx.amount);
                }
            }
            if (spend > 0) {
                CategoryTrend ct;
                ct.category_name = cat_info[i].first;
                ct.emoji = cat_info[i].second;
                ct.current_spend = spend;
                ct.percent_change = 0;
                category_trends.push_back(ct);
            }
        }

        Elements all_sections;

        all_sections.push_back(vbox({
            blue_text("  NET WORTH") | bold,
            hbox({ text("  "), DecorateAmount(net_worth) }),
            text(""),
            hbox({ text("  Checking:  ") | dim, DecorateAmount(checking) }),
            hbox({ text("  Savings:   ") | dim, DecorateAmount(savings) }),
            hbox({ text("  Credit:    ") | dim, text(format_currency(credit)) | color(Color::Red) | dim }),
            hbox({ text("  Invest:    ") | dim, DecorateAmount(investment) }),
            text(""),
            blue_text("  THIS MONTH") | bold,
            hbox({ text("  Income:  ") | dim, DecorateAmount(income) }),
            hbox({ text("  Spending:") | dim, text(format_currency(-spending)) | color(Color::Red) | bold }),
        }));

        if (!shovel_suppliers.empty()) {
            Elements shovel_rows;
            shovel_rows.push_back(blue_text("  SHOVEL INTELLIGENCE") | bold);
            for (const auto& s : shovel_suppliers) {
                shovel_rows.push_back(hbox({
                    text("  " + s.ticker) | bold | color(Color::Cyan),
                    text("  " + format_currency(s.amount)) | dim,
                }));
            }
            shovel_rows.push_back(hbox({
                text("  Score: ") | dim,
                text(std::to_string((int)shovel_score) + "/100") | bold | color(Color::Cyan),
                text("  (" + std::to_string(shovel_suppliers.size()) + " companies)") | dim
            }));
            all_sections.push_back(vbox(std::move(shovel_rows)));
        }

        if (!category_trends.empty()) {
            Elements cat_rows;
            cat_rows.push_back(blue_text("  TOP CATEGORIES") | bold);
            for (const auto& ct : category_trends) {
                cat_rows.push_back(hbox({
                    text("  " + ct.emoji + " " + ct.category_name) | dim,
                    text("  $" + format_currency(ct.current_spend)) | bold,
                }));
            }
            all_sections.push_back(vbox(std::move(cat_rows)));
        }

        return vbox(std::move(all_sections)) | flex;
    }

private:
    DataStore& data_store_;
    std::string entity_id_;
};