#pragma once

#include <string>
#include <string_view>

bool open_browser(std::string_view url);
bool sanitize_url(std::string_view url);
