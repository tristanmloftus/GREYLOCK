#pragma once

// V3ObjectsHandler.h — read-only GET routes for v3/v4 typed objects.
//
// Routes registered (all session-authenticated):
//   GET /decisions               — list (newest first)
//   GET /decisions/:id           — one decision
//   GET /relationships           — list (alphabetical)
//   GET /relationships/:id       — one relationship (id OR slug-matched name)
//   GET /targets                 — list (newest updated first)
//   GET /targets/:id             — one target
//
// Requires M008 (decisions, tasks, events, notes) + M009 (targets,
// relationships, real_estate) migrations.

namespace httplib { class SSLServer; }
class Database;

namespace tf::data {

void register_v3_objects_handlers(httplib::SSLServer& server,
                                   Database& db);

} // namespace tf::data
