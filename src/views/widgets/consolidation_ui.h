// consolidation_ui — promoted from proposals/ in Phase 5.
//
// Renders bank-connection status and a consolidated net-worth panel
// across linked accounts.  Currently not wired into DashboardView;
// kept available for future panel use (the ConsolidationService is
// being refactored in parallel by the discovery-engineer).

#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

struct AccountConnection {
    std::string institution;
    std::string account_name;
    double balance;
    bool connected;
    std::string last_sync;
};

} // namespace tf::widgets

namespace ftxui {

Component BankConnectionStatus(const std::vector<tf::widgets::AccountConnection>& accounts);

Element BankConnectionStatusRenderer(const std::vector<tf::widgets::AccountConnection>& accounts);

Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment);

Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment);

} // namespace ftxui
