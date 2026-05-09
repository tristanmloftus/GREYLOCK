#pragma once

// Forward-declare to avoid pulling httplib.h into every includer.
namespace httplib { class SSLServer; }

// Registers the GET /healthz route on the given server.
//
// Response: HTTP 200, Content-Type: application/json
// Body:     {"ok":true,"version":"0.2"}
//
// No authentication required — health checks must be reachable
// without credentials so load balancers / uptime monitors can use them.
void register_healthz_handler(httplib::SSLServer& server);
