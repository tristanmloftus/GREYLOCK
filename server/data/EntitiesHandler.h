#pragma once

// EntitiesHandler.h — /entities CRUD routes (Phase 4.B).
//
// Routes registered:
//   GET    /entities           — list entities the user is a member of
//   GET    /entities/<id>      — fetch one entity (403 if not member)
//   POST   /entities           — create entity; auto-creates owner membership
//   PUT    /entities/<id>      — update entity (403 if not member)
//   DELETE /entities/<id>      — delete entity (403 if not owner)
//
// All routes are session-authenticated via require_session().
// All routes are entity-membership-authorized via user_has_access_to_entity().

// Forward declarations.
namespace httplib { class SSLServer; }
class Database;
namespace tf::audit { class IAuditLog; }

namespace tf::data {

// Register all /entities routes on the given SSLServer.
// db and audit_log must outlive the server.
void register_entities_handlers(httplib::SSLServer& server,
                                 Database& db,
                                 tf::audit::IAuditLog& audit_log);

} // namespace tf::data
