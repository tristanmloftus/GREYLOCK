#include "consolidation_ui.h"

namespace ftxui {

Component BankConnectionStatus(const std::vector<AccountConnection>& accounts) {
    return Renderer([accounts] {
        return BankConnectionStatusRenderer(accounts);
    });
}

Element BankConnectionStatusRenderer(const std::vector<AccountConnection>& accounts) {
    std::vector<Element> rows;

    rows.push_back(text("Bank Connections") | bold);
    rows.push_back(separator());

    for (const auto& acc : accounts) {
        std::string status = acc.connected ? "[Connected]" : "[Disconnected]";
        std::string last_sync = acc.last_sync.empty() ? "Never" : acc.last_sync;

        Element row = hbox({
            text(status) | color(acc.connected ? Color::Green : Color::Red),
            text(" ") | hidden,
            text(acc.institution) | bold,
            text(" - ") | dim,
            text(acc.account_name),
            text("  $") | dim,
            text(std::to_string(acc.balance)) | bold,
            text("  Last sync: ") | dim,
            text(last_sync)
        });
        rows.push_back(row);
    }

    if (accounts.empty()) {
        rows.push_back(text("  No accounts connected.") | dim);
        rows.push_back(text("  Press [P] to link a bank account.") | dim);
    }

    return vbox(std::move(rows)) | border;
}

Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment) {
    return Renderer([net_worth, checking, savings, credit, investment] {
        return ConsolidatedNetWorthRenderer(net_worth, checking, savings, credit, investment);
    });
}

Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    std::vector<Element> rows;

    rows.push_back(text("Consolidated Net Worth") | bold);
    rows.push_back(separator());

    oss.str("");
    oss << "$" << net_worth;
    rows.push_back(text(oss.str()) | bold | color(net_worth >= 0 ? Color::Green : Color::Red));
    rows.push_back(text(""));

    rows.push_back(hbox({ text("Checking:  ") | dim, text("$" + std::to_string(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text("$" + std::to_string(savings)) }));
    rows.push_back(hbox({ text("Credit:     ") | dim, text("$" + std::to_string(credit)) }));
    rows.push_back(hbox({ text("Investment: ") | dim, text("$" + std::to_string(investment)) }));

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui