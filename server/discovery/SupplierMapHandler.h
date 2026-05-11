#pragma once

// SupplierMapHandler.h — registers GET /supplier-map (Phase 5).
//
// Serves the canonical supplier-recognition rules (data/supplier_map.json)
// to authenticated clients.  This lets the TUI refresh its in-memory map
// without a binary release.
//
// Auth: session-gated via SessionMiddleware::require_session().  Anonymous
//       requests get 401.  IP/XFF gating is NOT performed (F-5 guardrail).
//
// Errors: 500 with {"error":"supplier_map_unavailable"} on missing/malformed
//         JSON.  No detail leakage in the body.

#include <string>

namespace httplib { class SSLServer; }
class Database;

namespace tf::discovery {

// JSON path used by the handler.  Default points at data/supplier_map.json
// relative to the server's current working directory — the same path the
// DiscoveryService uses on boot.  Tests can override via the second arg.
void register_supplier_map_handler(httplib::SSLServer& server,
                                    Database& db,
                                    std::string json_path = "data/supplier_map.json");

} // namespace tf::discovery
