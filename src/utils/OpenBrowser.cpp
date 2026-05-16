#include "OpenBrowser.h"
#include "Logger.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

static const char* kAllowedHosts[] = {
    "cdn.plaid.com",
    "localhost",
    "127.0.0.1",
};

static bool is_allowed_host(std::string_view url) {
    for (const auto& host : kAllowedHosts) {
        if (url.substr(0, host.size()) == host) {
            char c = url.size() > host.size() ? url[host.size()] : '\0';
            if (c == '/' || c == ':' || c == '\0') return true;
        }
        std::string with_www = std::string("www.") + host;
        if (url.substr(0, with_www.size()) == with_www) {
            char c = url.size() > with_www.size() ? url[with_www.size()] : '\0';
            if (c == '/' || c == ':' || c == '\0') return true;
        }
    }
    return false;
}

bool sanitize_url(std::string_view url) {
    if (url.empty()) return false;

    if (url.substr(0, 8) != "https://" && url.substr(0, 7) != "http://") {
        return false;
    }

    size_t host_start = url.find("://") + 3;
    size_t host_end = url.find_first_of("/?#", host_start);
    if (host_end == std::string_view::npos) host_end = url.size();
    std::string_view host = url.substr(host_start, host_end - host_start);

    if (!is_allowed_host(host)) return false;

    for (size_t i = 0; i < url.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c < 32 && c != '\t') return false;
    }

    return true;
}

bool open_browser(std::string_view url) {
    if (!sanitize_url(url)) {
        Logger::instance().error(
            "open_browser: rejected unsafe URL");
        return false;
    }

    std::string cmd;
#ifdef _WIN32
    cmd = "start \"\" \"" + std::string(url) + "\"";
#elif defined(__APPLE__)
    cmd = "open \"" + std::string(url) + "\"";
#else
    cmd = "xdg-open \"" + std::string(url) + "\"";
#endif

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        Logger::instance().error(
            "open_browser: command failed with exit code " + std::to_string(ret));
        return false;
    }
    return true;
}
