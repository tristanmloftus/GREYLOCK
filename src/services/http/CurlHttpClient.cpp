#include "CurlHttpClient.h"
#include "../../utils/Logger.h"

#include <curl/curl.h>
#include <algorithm>
#include <stdexcept>
#include <string_view>

// --------------------------------------------------------------------------
// Global libcurl initializer
//
// curl_global_init / curl_global_cleanup must be called exactly once per
// process. We use a static local object whose constructor/destructor run at
// program startup and exit respectively. This is safe in a single-threaded
// startup scenario (which TerminalFinance is — main() starts before the TUI
// event loop, no threading at this stage).
// --------------------------------------------------------------------------
namespace {

struct CurlGlobal {
    CurlGlobal() {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            // Can't use Logger here (might not be initialized yet). Write to
            // stderr and throw — the application cannot safely use libcurl.
            throw std::runtime_error(
                std::string("curl_global_init failed: ") + curl_easy_strerror(rc));
        }
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

// Defined in an anonymous namespace so the destructor runs at static-object
// teardown, after all CurlHttpClient instances have been destroyed.
static CurlGlobal g_curl_global;

// --------------------------------------------------------------------------
// CRLF guard for request headers.
//
// HTTP header injection (CRLF smuggling) lets an attacker terminate the
// current header and inject arbitrary headers — or split the request — by
// passing "\r\n" inside a caller-supplied header name or value.  We reject
// any such header outright instead of trying to sanitize.
// --------------------------------------------------------------------------
static bool contains_crlf(std::string_view s) {
    return s.find('\r') != std::string_view::npos
        || s.find('\n') != std::string_view::npos;
}

// --------------------------------------------------------------------------
// Write callback — appends received data to a std::string*.
// --------------------------------------------------------------------------
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}

// --------------------------------------------------------------------------
// Header callback — parses "Name: Value\r\n" lines into a map.
// The status-line ("HTTP/1.1 200 OK") is silently ignored.
// --------------------------------------------------------------------------
size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    std::string line(ptr, total);
    // Trim trailing \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }

    auto colon = line.find(':');
    if (colon == std::string::npos) {
        // Status-line or empty line — skip
        return total;
    }

    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);

    // Trim leading whitespace from value
    auto val_start = value.find_first_not_of(' ');
    if (val_start != std::string::npos) {
        value = value.substr(val_start);
    } else {
        value.clear();
    }

    // Case-preserving storage for Phase 0; last value wins for duplicate names.
    (*headers)[name] = value;
    return total;
}

} // anonymous namespace

// --------------------------------------------------------------------------
// CurlHttpClient implementation
// --------------------------------------------------------------------------

CurlHttpClient::CurlHttpClient() {
    curl_ = curl_easy_init();
    if (!curl_) {
        throw std::runtime_error("curl_easy_init() returned null — libcurl init failed");
    }
}

CurlHttpClient::~CurlHttpClient() {
    if (curl_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
}

std::optional<HttpResponse> CurlHttpClient::send(const HttpRequest& req) {
    CURL* curl = static_cast<CURL*>(curl_);

    // Reset all options to their defaults before each call. This is necessary
    // because we reuse the easy handle for connection keep-alive but each
    // request is logically independent.
    curl_easy_reset(curl);

    // -- TLS: always verify peer certificate and hostname. Never configurable. --
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // -- Optional CA bundle override (Phase 2: integration tests + dev certs) --
    // When ca_bundle_path is set, libcurl uses that file as the sole trust
    // anchor instead of the system bundle.  TLS verification is NOT weakened —
    // this is a trust-anchor swap, not a verify bypass.
    if (req.ca_bundle_path.has_value()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, req.ca_bundle_path->c_str());
    }

    // -- Timeouts (both connect and total transfer) --
    long timeout_ms = static_cast<long>(req.timeout.count());
    // Connect timeout: at most half the total timeout, minimum 5 s.
    long connect_ms = std::max(5'000L, timeout_ms / 2);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    // -- User-Agent --
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TerminalFinance/0.2");

    // -- URL --
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());

    // -- Follow redirects (up to 10) --
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    // -- Protocol allowlist: HTTPS only, for both the initial request and any
    //    redirect target.  Defense-in-depth: refuse http:// even if a server
    //    tries to redirect us to it.  F-2 / supply-chain hardening — we never
    //    follow a 30x into plaintext HTTP.
    //
    //    CURLOPT_*_STR forms require libcurl 7.85+ (0x075500); older versions
    //    use the bit-mask CURLOPT_PROTOCOLS / CURLOPT_REDIR_PROTOCOLS.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,       "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,       CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif

    // -- Response body capture --
    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // -- Response header capture --
    HttpResponse resp;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);

    // -- Request headers --
    //
    // Reject CRLF in header NAME or VALUE — this is HTTP-header-injection
    // (smuggling).  The header name is logged for diagnostics; the value is
    // NEVER logged because it may carry a secret (Authorization bearer token,
    // session cookie, etc.).
    curl_slist* header_list = nullptr;
    for (const auto& [name, value] : req.headers) {
        if (contains_crlf(name) || contains_crlf(value)) {
            Logger::instance().error(
                std::string("CurlHttpClient: rejecting CRLF in header: ") + name);
            if (header_list) {
                curl_slist_free_all(header_list);
            }
            return std::nullopt;
        }
        std::string header_line = name + ": " + value;
        header_list = curl_slist_append(header_list, header_line.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // -- HTTP method and body --
    const std::string& method = req.method;
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req.body.has_value()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body->size()));
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        }
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (req.body.has_value()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body->size()));
        }
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (req.body.has_value()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body->size()));
        }
    } else {
        // Default: GET (also handles HEAD, PATCH via CUSTOMREQUEST if needed later)
        if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        // No body for GET
    }

    // -- Execute --
    CURLcode rc = curl_easy_perform(curl);

    // Clean up headers list regardless of outcome
    if (header_list) {
        curl_slist_free_all(header_list);
    }

    if (rc != CURLE_OK) {
        Logger::instance().error(
            "CurlHttpClient: request failed [" + method + " " + req.url + "]: " +
            curl_easy_strerror(rc));
        return std::nullopt;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    resp.status_code = http_code;
    resp.body = std::move(response_body);

    Logger::instance().info(
        "CurlHttpClient: " + method + " " + req.url +
        " -> " + std::to_string(http_code));

    return resp;
}
