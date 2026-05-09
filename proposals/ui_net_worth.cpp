#include "ui_net_worth.h"
#include <sstream>
#include <iomanip>

namespace ftxui {

Component NetWorthBreakdown(double checking, double savings, double credit, double investment, double net_worth) {
    return Renderer([=] {
        return NetWorthBreakdownRenderer(checking, savings, credit, investment, net_worth);
    });
}

Element NetWorthBreakdownRenderer(double checking, double savings, double credit, double investment, double net_worth) {
    auto format_currency = [](double val) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "$" << val;
        return oss.str();
    };

    std::vector<Element> rows;

    rows.push_back(text("Net Worth") | bold);
    rows.push_back(separator());

    Color net_color = net_worth >= 0 ? Color::Green : Color::Red;
    rows.push_back(text(format_currency(net_worth)) | bold | color(net_color));
    rows.push_back(text(""));

    auto acc_row = [](const std::string& label, double val, Color c) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        return hbox({
            text(label) | dim,
            text("  ") | hidden,
            text(oss.str() + "$" + std::to_string(val)) | color(c)
        });
    };

    rows.push_back(hbox({ text("Checking:   ") | dim, text(format_currency(checking)) }));
    rows.push_back(hbox({ text("Savings:    ") | dim, text(format_currency(savings)) }));
    rows.push_back(hbox({ text("Credit:     ") | dim, text(format_currency(credit)) | color(credit < 0 ? Color::Red : Color::Default) }));
    rows.push_back(hbox({ text("Investment: ") | dim, text(format_currency(investment)) }));

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui