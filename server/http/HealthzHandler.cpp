#include "httplib.h"
#include "HealthzHandler.h"

// GET /healthz — returns {"ok":true,"version":"0.2"} with status 200.
//
// No auth, no rate-limiting.  Intentional: health checks must be reachable
// by load balancers and uptime monitors without credentials.
//
// The JSON body is a compile-time constant — no dynamic allocation at request
// time.  Content-Type is set explicitly so clients can rely on it (tested in
// ServerHealthzTest::Healthz_ContentTypeIsJson).
void register_healthz_handler(httplib::SSLServer& server) {
    server.Get("/healthz", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(R"({"ok":true,"version":"0.2"})", "application/json");
        // status_code defaults to 200 in cpp-httplib when set_content() is called;
        // setting it explicitly removes any ambiguity for readers and tests.
        res.status = 200;
    });
}
