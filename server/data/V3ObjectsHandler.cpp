// V3ObjectsHandler.cpp — GET routes for decisions / relationships / targets.
//
// Minimal read-only surface so the TUI's reference-design views
// (DecisionDetailView, RelationshipDetailView, GraphView) can pull
// real data without an ingestion pipeline.  Writes come later with
// vault ingestion + the `:new <type>` palette command.

#include "httplib.h"

#include "V3ObjectsHandler.h"
#include "../auth/SessionMiddleware.h"
#include "../db/Database.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace tf::data {

namespace {

void send_json_v3(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Tolerant id lookup: accept exact id, or a slugged display_name match.
// We collapse non-alphanumeric runs to '-' and lower-case for the slug
// so `:open cade` matches "Cade Hartford" -> "cade-hartford" matches.
std::string slug(const std::string& s) {
    std::string out;
    bool last_dash = true;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out += static_cast<char>(std::tolower(u));
            last_dash = false;
        } else if (!last_dash) {
            out += '-';
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// ---- row → json helpers ----

json decision_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]                   = T(0);
    o["title"]                = T(1);
    o["body_md"]              = T(2);
    o["entity_scope"]         = T(3);
    o["status"]               = T(4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL) o["outcome"] = T(5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["decided_by_user_id"] = T(6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL)
        o["decided_at_unix"]  = sqlite3_column_int64(s, 7);
    if (sqlite3_column_type(s, 8) != SQLITE_NULL)
        o["outcome_due_unix"] = sqlite3_column_int64(s, 8);
    if (sqlite3_column_type(s, 9) != SQLITE_NULL) o["source_note_id"] = T(9);
    o["created_at_unix"]      = sqlite3_column_int64(s, 10);
    return o;
}

json relationship_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]                = T(0);
    o["display_name"]      = T(1);
    o["kind"]              = T(2);
    if (sqlite3_column_type(s, 3) != SQLITE_NULL) o["primary_email"] = T(3);
    if (sqlite3_column_type(s, 4) != SQLITE_NULL) o["primary_phone"] = T(4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL)
        o["last_contact_unix"] = sqlite3_column_int64(s, 5);
    o["notes_md"]          = T(6);
    o["created_at_unix"]   = sqlite3_column_int64(s, 7);
    o["updated_at_unix"]   = sqlite3_column_int64(s, 8);
    return o;
}

json target_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]                = T(0);
    o["name"]              = T(1);
    o["entity_scope"]      = T(2);
    o["stage"]             = T(3);
    o["thesis_md"]         = T(4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL)
        o["size_cents"] = sqlite3_column_int64(s, 5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["source_note_id"] = T(6);
    o["created_at_unix"]   = sqlite3_column_int64(s, 7);
    o["updated_at_unix"]   = sqlite3_column_int64(s, 8);
    return o;
}

// ---- handlers ----

void handle_list_decisions(const httplib::Request& req,
                           httplib::Response& res,
                           Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, outcome, "
        "       decided_by_user_id, decided_at_unix, outcome_due_unix, "
        "       source_note_id, created_at_unix "
        "FROM decisions ORDER BY COALESCE(decided_at_unix, created_at_unix) DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(decision_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_decision(const httplib::Request& req,
                         httplib::Response& res,
                         Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");

    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, outcome, "
        "       decided_by_user_id, decided_at_unix, outcome_due_unix, "
        "       source_note_id, created_at_unix "
        "FROM decisions WHERE id = ? OR title = ? LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, decision_row_to_json(stmt.get()));
}

void handle_list_relationships(const httplib::Request& req,
                               httplib::Response& res,
                               Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, display_name, kind, primary_email, primary_phone, "
        "       last_contact_unix, notes_md, created_at_unix, updated_at_unix "
        "FROM relationships ORDER BY display_name ASC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(relationship_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_relationship(const httplib::Request& req,
                             httplib::Response& res,
                             Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id  = req.path_params.at("id");
    std::string sl  = slug(id);

    // Try exact id, then exact display_name, then slugged display_name.
    auto stmt = db.prepare(
        "SELECT id, display_name, kind, primary_email, primary_phone, "
        "       last_contact_unix, notes_md, created_at_unix, updated_at_unix "
        "FROM relationships "
        "WHERE id = ? OR LOWER(display_name) = LOWER(?) "
        "ORDER BY id LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);

    if (stmt.step() == SQLITE_ROW) {
        send_json_v3(res, 200, relationship_row_to_json(stmt.get()));
        return;
    }

    // Slug fallback: walk rows once, score each, emit the best match.
    // Score:  3 = slug exact ("cade-hartford" == "cade-hartford")
    //         2 = slug token  ("cade" matches "cade-hartford" via '-' split)
    //         1 = case-insensitive substring on display_name
    auto stmt2 = db.prepare(
        "SELECT id, display_name, kind, primary_email, primary_phone, "
        "       last_contact_unix, notes_md, created_at_unix, updated_at_unix "
        "FROM relationships;");
    json best_match = json();
    int  best_score = 0;
    std::string lowered_id = lower(id);
    while (stmt2.step() == SQLITE_ROW) {
        const char* dn_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt2.get(), 1));
        if (!dn_c) continue;
        std::string dn  = dn_c;
        std::string dns = slug(dn);

        int score = 0;
        if (dns == sl) {
            score = 3;
        } else {
            std::size_t start = 0;
            while (start < dns.size()) {
                std::size_t dash = dns.find('-', start);
                if (dash == std::string::npos) dash = dns.size();
                if (dns.substr(start, dash - start) == sl) { score = 2; break; }
                start = dash + 1;
            }
            if (score == 0 && !sl.empty()
                && lower(dn).find(lowered_id) != std::string::npos) {
                score = 1;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_match = relationship_row_to_json(stmt2.get());
            if (score == 3) break;
        }
    }
    if (best_score > 0 && !best_match.is_null()) {
        send_json_v3(res, 200, best_match);
        return;
    }
    send_json_v3(res, 404, {{"error", "not_found"}});
}

void handle_list_targets(const httplib::Request& req,
                         httplib::Response& res,
                         Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, name, entity_scope, stage, thesis_md, size_cents, "
        "       source_note_id, created_at_unix, updated_at_unix "
        "FROM targets ORDER BY updated_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(target_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_target(const httplib::Request& req,
                       httplib::Response& res,
                       Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");

    auto stmt = db.prepare(
        "SELECT id, name, entity_scope, stage, thesis_md, size_cents, "
        "       source_note_id, created_at_unix, updated_at_unix "
        "FROM targets WHERE id = ? OR LOWER(name) = LOWER(?) LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, target_row_to_json(stmt.get()));
}

}  // namespace

void register_v3_objects_handlers(httplib::SSLServer& server, Database& db) {
    server.Get("/decisions",       [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_decisions(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/decisions/:id",   [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_decision(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/relationships",   [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_relationships(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/relationships/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_relationship(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/targets",         [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_targets(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/targets/:id",     [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_target(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
}

}  // namespace tf::data
