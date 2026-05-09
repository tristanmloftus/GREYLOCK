#pragma once

#include "../IHttpClient.h"

// libcurl-backed implementation of IHttpClient.
//
// Initialization: curl_global_init(CURL_GLOBAL_DEFAULT) is called once at
// program startup via a static initializer in CurlHttpClient.cpp. The
// corresponding curl_global_cleanup() runs at exit. Neither is called
// per-request.
//
// TLS: CURLOPT_SSL_VERIFYPEER and CURLOPT_SSL_VERIFYHOST are both ON.
// There is no way to disable verification — not even at compile time.
//
// Timeouts: both connect timeout (CURLOPT_CONNECTTIMEOUT_MS) and total
// transfer timeout (CURLOPT_TIMEOUT_MS) are set from req.timeout on every
// call. No request can hang forever.
//
// User-Agent: "TerminalFinance/0.2" sent on every request.
//
// Supported methods: GET, POST, PUT, DELETE.
class CurlHttpClient : public IHttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    std::optional<HttpResponse> send(const HttpRequest& req) override;

private:
    // Non-copyable, non-movable. The CURL handle is per-instance.
    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;
    CurlHttpClient(CurlHttpClient&&) = delete;
    CurlHttpClient& operator=(CurlHttpClient&&) = delete;

    // Per-instance easy handle. Reused across calls for connection keep-alive.
    void* curl_{nullptr};  // CURL* — stored as void* to avoid pulling curl.h into the header
};
