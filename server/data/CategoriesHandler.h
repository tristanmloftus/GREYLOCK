#pragma once

// CategoriesHandler.h — /categories CRUD routes (Phase 4.B).
//
// Routes registered:
//   GET    /entities/<entity_id>/categories  — list categories for an entity
//   GET    /categories/<id>                  — fetch one category
//   POST   /entities/<entity_id>/categories  — create a category
//   PUT    /categories/<id>                  — update a category
//   DELETE /categories/<id>                  — delete a category
//
// Requires M002 migration (categories table).
// All routes are session-authenticated via require_session().

// Forward declarations.
namespace httplib { class SSLServer; }
class Database;
namespace tf::audit { class IAuditLog; }

namespace tf::data {

void register_categories_handlers(httplib::SSLServer& server,
                                   Database& db,
                                   tf::audit::IAuditLog& audit_log);

} // namespace tf::data
