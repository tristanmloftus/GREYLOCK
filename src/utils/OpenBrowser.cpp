// OpenBrowser.cpp — secure URL → browser opener for the Plaid Link flow.
//
// Security model:
//   1. sanitize_url() rejects non-https, unknown hosts, control characters,
//      and shell metacharacters. Allowed hosts: the host of TF_BACKEND_URL
//      plus localhost / 127.0.0.1 (dev fallback).
//   2. open_browser() invokes the platform browser-launcher via argv-based
//      exec (posix_spawnp on POSIX, ShellExecuteA on Windows). The URL is
//      passed as a single argv element — the shell is NEVER involved.
//      Command-injection via $(...), backticks, ;, |, etc. is structurally
//      impossible.
//
// History: this file previously used std::system("xdg-open \"...url...\"")
// which was vulnerable to shell injection through URL query parameters.
// Fixed 2026-05-16.

#include "OpenBrowser.h"
#include "Logger.h"

#include <cstdlib>
#include <string>
#include <string_view>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
  extern char **environ;
#endif

namespace {

// Extract the host portion (with optional :port) from a URL.
// Returns empty string if the URL is malformed.
std::string extract_host_and_port(std::string_view url) {
    if (url.size() < 8) return "";
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return "";
    size_t host_start = scheme_end + 3;
    size_t host_end = url.find_first_of("/?#", host_start);
    if (host_end == std::string_view::npos) host_end = url.size();
    return std::string(url.substr(host_start, host_end - host_start));
}

// Strip optional :port suffix.
std::string strip_port(const std::string& host_with_port) {
    size_t colon = host_with_port.find(':');
    if (colon == std::string::npos) return host_with_port;
    return host_with_port.substr(0, colon);
}

// Pull the host (without port) from TF_BACKEND_URL env var, if set.
std::string backend_host_from_env() {
    const char* backend_url = std::getenv("TF_BACKEND_URL");
    if (!backend_url || backend_url[0] == '\0') return "";
    return strip_port(extract_host_and_port(backend_url));
}

// Pull the host (without port) from TF_BROWSER_URL env var, if set.
// This is the URL the user's browser hits when the TUI itself runs on
// a remote host (the Tailscale IP of that host, typically).
std::string browser_host_from_env() {
    const char* browser_url = std::getenv("TF_BROWSER_URL");
    if (!browser_url || browser_url[0] == '\0') return "";
    return strip_port(extract_host_and_port(browser_url));
}

} // namespace

bool sanitize_url(std::string_view url) {
    if (url.empty()) return false;

    // HTTPS only — even local dev uses TLS via mkcert.
    if (url.size() < 8 || url.substr(0, 8) != "https://") return false;

    // Reject control characters AND shell metacharacters.
    //
    // open_browser() uses argv-based exec, so the shell is never invoked —
    // these characters cannot cause command injection through that path.
    // We reject them here anyway:
    //   (a) defense in depth against any future regression to a shell-based
    //       opener
    //   (b) URL hygiene: Plaid Link URLs never contain raw $, `, ;, |, etc.
    //
    // Note: & = and ? are intentionally NOT rejected — they are valid URL
    // query-string syntax (e.g. ?account_id=x&link_token=y).
    for (unsigned char c : url) {
        if (c < 32 && c != '\t') return false;
        switch (c) {
            case '$': case '`': case '\\': case '"': case '\'':
            case ';': case '|':
                return false;
            default:
                break;
        }
    }

    std::string host_with_port = extract_host_and_port(url);
    if (host_with_port.empty()) return false;
    std::string host = strip_port(host_with_port);

    // Allowed hosts:
    //   1. The host of TF_BACKEND_URL (the server we talk to).
    //   2. The host of TF_BROWSER_URL (when set, this is the URL the
    //      user's browser hits — e.g. the Tailscale IP of a remote
    //      server hosting the TUI's backend).
    //   3. localhost / 127.0.0.1 (dev fallback if neither env is set).
    std::string backend_host = backend_host_from_env();
    if (!backend_host.empty() && host == backend_host) return true;
    std::string browser_host = browser_host_from_env();
    if (!browser_host.empty() && host == browser_host) return true;
    if (host == "localhost") return true;
    if (host == "127.0.0.1") return true;

    return false;
}

bool open_browser(std::string_view url) {
    if (!sanitize_url(url)) {
        Logger::instance().error("open_browser: rejected unsafe URL");
        return false;
    }

    std::string url_str(url);

#ifdef _WIN32
    // ShellExecuteA accepts lpFile as the URL directly. Windows dispatches
    // to the default browser via URL protocol handler. No shell parsing of
    // the URL string.
    HINSTANCE result = ShellExecuteA(
        nullptr, "open", url_str.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteA returns a value > 32 on success.
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        Logger::instance().error(
            "open_browser: ShellExecuteA failed for browser dispatch");
        return false;
    }
    return true;
#else
  #ifdef __APPLE__
    const char* opener = "open";
  #else
    const char* opener = "xdg-open";
  #endif

    // posix_spawnp with an explicit argv array: the child invokes the opener
    // directly via execve. The URL is one argv element — the shell is never
    // invoked.
    char* argv[] = {
        const_cast<char*>(opener),
        const_cast<char*>(url_str.c_str()),
        nullptr
    };
    pid_t pid;
    int rc = posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ);
    if (rc != 0) {
        Logger::instance().error(
            "open_browser: posix_spawnp failed for browser dispatch");
        return false;
    }
    // Fire-and-forget: don't waitpid. The browser process detaches; init
    // reaps it when the user closes it.
    return true;
#endif
}
