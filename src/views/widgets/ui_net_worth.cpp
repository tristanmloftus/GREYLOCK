#include "ui_net_worth.h"

#include <iomanip>
#include <sstream>

namespace ftxui {

namespace {
std::string format_currency(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (val < 0) {
        oss << "-$" << (-val);
    } else {
        oss << "$" << val;
    }
    return oss.str();
}
}  // namespace

Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth) {
    return Renderer([=] {
        return NetWorthBreakdownRenderer(checking, savings, credit, investment, net_worth);
    });
}

Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth) {
    std::vector<Element> rows;

    rows.push_back(text("Net Worth") | bold);
    rows.push_back(separator());

    const Color net_color = net_worth >= 0 ? Color::Green : Color::Red;
    rows.push_back(text(format_currency(net_worth)) | bold | color(net_color));
    rows.push_back(text(""));

    rows.push_back(hbox({ text("Checking:   ") | dim, text(format_currency(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text(format_currency(savings)) }));
    // Credit balances are negative liabilities; color in red only when balance < 0.
    Element credit_value = text(format_currency(credit));
    if (credit < 0) credit_value = credit_value | color(Color::Red);
    rows.push_back(hbox({ text("Credit:     ") | dim, credit_value }));
    rows.push_back(hbox({ text("Investment: ") | dim, text(format_currency(investment)) }));

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
