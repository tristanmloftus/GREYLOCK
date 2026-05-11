#include "consolidation_ui.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {
std::string format_amount(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val;
    return oss.str();
}
}  // namespace

Component BankConnectionStatus(const std::vector<tf::widgets::AccountConnection>& accounts) {
    return Renderer([accounts] {
        return BankConnectionStatusRenderer(accounts);
    });
}

Element BankConnectionStatusRenderer(const std::vector<tf::widgets::AccountConnection>& accounts) {
    std::vector<Element> rows;

    rows.push_back(text("Bank Connections") | bold);
    rows.push_back(separator());

    if (accounts.empty()) {
        rows.push_back(text("  No accounts connected.") | dim);
        rows.push_back(text("  Press [P] to link a bank account.") | dim);
    } else {
        for (const auto& acc : accounts) {
            const std::string status = acc.connected ? "[Connected]" : "[Disconnected]";
            const std::string last_sync = acc.last_sync.empty() ? "Never" : acc.last_sync;

            Element row = hbox({
                text(status) | color(acc.connected ? Color::Green : Color::Red),
                text(" "),
                text(acc.institution) | bold,
                text(" - ") | dim,
                text(acc.account_name),
                text("  $") | dim,
                text(format_amount(acc.balance)) | bold,
                text("  Last sync: ") | dim,
                text(last_sync)
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

Component ConsolidatedNetWorth(double net_worth, double checking, double savings, double credit, double investment) {
    return Renderer([=] {
        return ConsolidatedNetWorthRenderer(net_worth, checking, savings, credit, investment);
    });
}

Element ConsolidatedNetWorthRenderer(double net_worth, double checking, double savings, double credit, double investment) {
    std::vector<Element> rows;

    rows.push_back(text("Consolidated Net Worth") | bold);
    rows.push_back(separator());

    rows.push_back(text("$" + format_amount(net_worth)) | bold | color(net_worth >= 0 ? Color::Green : Color::Red));
    rows.push_back(text(""));

    rows.push_back(hbox({ text("Checking:   ") | dim, text("$" + format_amount(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text("$" + format_amount(savings)) }));
    rows.push_back(hbox({ text("Credit:     ") | dim, text("$" + format_amount(credit)) }));
    rows.push_back(hbox({ text("Investment: ") | dim, text("$" + format_amount(investment)) }));

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
