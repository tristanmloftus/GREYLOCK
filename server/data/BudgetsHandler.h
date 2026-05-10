#pragma once

// BudgetsHandler.h — /budgets CRUD routes (Phase 4.B).
//
// Routes registered:
//   GET    /entities/<entity_id>/budgets  — list budgets for an entity
//   GET    /budgets/<id>                  — fetch one budget
//   POST   /entities/<entity_id>/budgets  — create a budget
//   PUT    /budgets/<id>                  — update a budget
//   DELETE /budgets/<id>                  — delete a budget
//
// Requires M003 migration (budgets table).
// All routes are session-authenticated via require_session().

// Forward declarations.
namespace httplib { class SSLServer; }
class Database;
namespace tf::audit { class IAuditLog; }

namespace tf::data {

void register_budgets_handlers(httplib::SSLServer& server,
                                Database& db,
                                tf::audit::IAuditLog& audit_log);

} // namespace tf::data
