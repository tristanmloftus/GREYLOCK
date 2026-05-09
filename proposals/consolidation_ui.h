#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <vector>
#include <string>

struct AccountConnection {
    std::string institution;
    std::string account_name;
    double balance;
    bool connected;
    std::string last_sync;
};

namespace ftxui {

Component BankConnectionStatus(const std::vector<AccountConnection>& accounts);

Element BankConnectionStatusRenderer(const std::vector<AccountConnection>& accounts);

Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment);

Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment);

} // namespace ftxui