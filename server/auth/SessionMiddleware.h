#pragma once

// SessionMiddleware.h — session extraction + validation helper (Phase 4.B).
//
// Every data endpoint uses require_session() to gate access.
// Returns user_id on success, nullopt on any auth failure.
// Callers: if (!user_id) { res.status = 401; return; }

#include <optional>
#include <string>

// Forward declarations to avoid pulling httplib.h and Database.h into consumers
// that don't need them directly.
namespace httplib { class Request; }
class Database;

namespace tf::data {

// Extract + validate the Bearer session token from the Authorization header.
// Calls Session::validate_and_touch internally.
// Returns the validated user_id on success.
// Returns nullopt if: header absent, token invalid, or session expired/revoked.
std::optional<std::string> require_session(const httplib::Request& req,
                                            Database& db);

} // namespace tf::data
