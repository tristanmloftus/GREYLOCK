// SessionMiddleware.cpp — session extraction + validation (Phase 4.B).

#include "httplib.h"

#include "SessionMiddleware.h"
#include "Session.h"
#include "../../server/db/Database.h"

#include <chrono>
#include <optional>
#include <string>

namespace tf::data {

std::optional<std::string> require_session(const httplib::Request& req,
                                            Database& db)
{
    // Extract Bearer token from Authorization header.
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
        return std::nullopt;
    }
    const std::string& hdr = it->second;
    if (hdr.size() < 8 || hdr.substr(0, 7) != "Bearer ") {
        return std::nullopt;
    }
    std::string raw_token = hdr.substr(7);

    int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Validate and touch the session; returns user_id or nullopt.
    return tf::auth::validate_and_touch_session(db, raw_token, now);
}

} // namespace tf::data
