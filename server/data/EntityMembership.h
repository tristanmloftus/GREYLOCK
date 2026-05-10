#pragma once

// EntityMembership.h — authorization helper for entity-scoped resources (Phase 4.B).
//
// Every data endpoint that touches an entity_id or entity-scoped resource calls
// user_has_access_to_entity() to verify the requesting user is a member.
// F-2: the check ALWAYS queries the DB; user-supplied entity_id is never trusted
// blindly as authorization evidence.

#include <string>
#include <string_view>

class Database;

namespace tf::data {

// Returns true iff entity_memberships has a row for (user_id, entity_id).
// Queries the DB every call — no in-process cache.
bool user_has_access_to_entity(Database& db,
                                std::string_view user_id,
                                std::string_view entity_id);

// Returns true iff the user has role 'owner' in entity_memberships.
bool user_is_owner_of_entity(Database& db,
                              std::string_view user_id,
                              std::string_view entity_id);

} // namespace tf::data
