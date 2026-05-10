// EntityMembership.cpp — entity membership authorization queries (Phase 4.B).

#include "EntityMembership.h"
#include "../../server/db/Database.h"

#include <sqlite3.h>

#include <string>
#include <string_view>

namespace tf::data {

bool user_has_access_to_entity(Database& db,
                                std::string_view user_id,
                                std::string_view entity_id)
{
    auto stmt = db.prepare(
        "SELECT 1 FROM entity_memberships "
        "WHERE user_id = ? AND entity_id = ? LIMIT 1;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        user_id.data(), static_cast<int>(user_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    return stmt.step() == SQLITE_ROW;
}

bool user_is_owner_of_entity(Database& db,
                              std::string_view user_id,
                              std::string_view entity_id)
{
    auto stmt = db.prepare(
        "SELECT 1 FROM entity_memberships "
        "WHERE user_id = ? AND entity_id = ? AND role = 'owner' LIMIT 1;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        user_id.data(), static_cast<int>(user_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    return stmt.step() == SQLITE_ROW;
}

} // namespace tf::data
