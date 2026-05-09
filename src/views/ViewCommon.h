#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <iomanip>
#include <sstream>

using namespace ftxui;

const Color LED_BLUE = Color(39, 170, 255);
const Color LED_BLUE_DIM = Color(20, 85, 135);

inline std::string format_currency(double amount) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (amount < 0) {
        ss << "-$" << std::abs(amount);
    } else {
        ss << "$" << amount;
    }
    return ss.str();
}

inline Element DecorateAmount(double val) {
    return text(format_currency(val)) | color(val >= 0 ? Color::Green : Color::Red) | bold;
}

inline Element blue_text(const std::string& s) {
    return text(s) | color(LED_BLUE);
}

inline Element blue_dim(const std::string& s) {
    return text(s) | color(LED_BLUE_DIM);
}

inline Element blue_bold(const std::string& s) {
    return text(s) | bold | color(LED_BLUE);
}