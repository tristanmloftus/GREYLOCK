#pragma once

// TransactionsHandler.h — /transactions CRUD routes (Phase 4.B).
//
// description_encrypted: v0.2 stores description as raw bytes. Phase 4.C adds encryption.

namespace httplib { class SSLServer; }
class Database;
namespace tf::audit { class IAuditLog; }

namespace tf::data {

void register_transactions_handlers(httplib::SSLServer& server,
                                     Database& db,
                                     tf::audit::IAuditLog& audit_log);

} // namespace tf::data
