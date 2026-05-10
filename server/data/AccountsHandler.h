#pragma once

// AccountsHandler.h — /accounts CRUD routes (Phase 4.B).
//
// SECURITY: accounts.encrypted_token is NEVER returned in any response.

// Forward declarations.
namespace httplib { class SSLServer; }
class Database;
namespace tf::audit { class IAuditLog; }

namespace tf::data {

void register_accounts_handlers(httplib::SSLServer& server,
                                 Database& db,
                                 tf::audit::IAuditLog& audit_log);

} // namespace tf::data
