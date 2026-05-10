// CategoriesHandler.cpp — /categories CRUD route implementations (Phase 4.B).
//
// GUARDRAILS:
//   F-2: every route calls require_session(); every entity-scoped route calls
//        user_has_access_to_entity() — user-supplied entity_id never trusted.
//   F-3: every handler wrapped in try/catch.
//
// Requires M002 migration (categories table):
//   id TEXT PK, entity_id TEXT NOT NULL, name TEXT NOT NULL, kind TEXT NOT NULL

#include "httplib.h"

#include "CategoriesHandler.h"
#include "EntityMembership.h"
#include "../auth/SessionMiddleware.h"
#include "../db/Database.h"
#include "../audit/IAuditLog.h"
#include "../audit/AuditEvent.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace tf::data {

static void send_json_cat(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static int64_t now_ms_cat() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string generate_id_cat() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

static void emit_audit_cat(tf::audit::IAuditLog& log,
                            const std::string& actor_user_id,
                            const std::string& domain,
                            const std::string& subject_id,
                            const std::string& action,
                            const std::string& outcome,
                            const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = now_ms_cat();
    evt.actor_user_id = actor_user_id;
    evt.actor_kind    = "user";
    evt.domain        = domain;
    evt.subject_id    = subject_id;
    evt.subject_kind  = "category";
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;
    log.record(evt);
}

// Columns: id, entity_id, name, kind
static json category_row_to_json(sqlite3_stmt* stmt) {
    json obj;
    const char* id        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* entity_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* name      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const char* kind      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    obj["id"]        = id        ? id        : "";
    obj["entity_id"] = entity_id ? entity_id : "";
    obj["name"]      = name      ? name      : "";
    obj["kind"]      = kind      ? kind      : "";
    return obj;
}

static std::string get_category_entity_id(Database& db, const std::string& cat_id) {
    auto stmt = db.prepare("SELECT entity_id FROM categories WHERE id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* eid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return eid ? eid : "";
    }
    return "";
}

// GET /entities/<entity_id>/categories
static void handle_list_categories(const httplib::Request& req,
                                    httplib::Response& res,
                                    Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_cat(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_cat(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, name, kind FROM categories WHERE entity_id = ? ORDER BY name ASC;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    json items = json::array();
    while (stmt.step() == SQLITE_ROW) {
        items.push_back(category_row_to_json(stmt.get()));
    }

    send_json_cat(res, 200, {{"items", items}});
}

// GET /categories/<id>
static void handle_get_category(const httplib::Request& req,
                                 httplib::Response& res,
                                 Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_cat(res, 401, {{"error", "unauthorized"}}); return; }

    std::string cat_id = req.path_params.at("id");

    std::string entity_id = get_category_entity_id(db, cat_id);
    if (entity_id.empty()) {
        send_json_cat(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_cat(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, name, kind FROM categories WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json_cat(res, 404, {{"error", "not_found"}});
        return;
    }

    send_json_cat(res, 200, category_row_to_json(stmt.get()));
}

// POST /entities/<entity_id>/categories
static void handle_create_category(const httplib::Request& req,
                                    httplib::Response& res,
                                    Database& db,
                                    tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_cat(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_cat(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_cat(res, 400, {{"error", "invalid_json"}}); return; }

    std::string name, kind;
    try {
        name = body.at("name").get<std::string>();
        kind = body.at("kind").get<std::string>();
    } catch (...) {
        send_json_cat(res, 400, {{"error", "missing_fields"},
                                  {"message", "name, kind required"}});
        return;
    }

    std::string cat_id = generate_id_cat();

    auto ins = db.prepare(
        "INSERT INTO categories (id, entity_id, name, kind) VALUES (?, ?, ?, ?);"
    );
    sqlite3_bind_text(ins.get(), 1, cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 2, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 3, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 4, kind.data(), static_cast<int>(kind.size()), SQLITE_STATIC);

    if (ins.step() != SQLITE_DONE) {
        send_json_cat(res, 500, {{"error", "internal"}});
        return;
    }

    emit_audit_cat(audit_log, *user_id, entity_id, cat_id,
                   "category_created", "success",
                   {{"name", name}, {"kind", kind}});

    send_json_cat(res, 201, {
        {"id", cat_id},
        {"entity_id", entity_id},
        {"name", name},
        {"kind", kind}
    });
}

// PUT /categories/<id>
static void handle_update_category(const httplib::Request& req,
                                    httplib::Response& res,
                                    Database& db,
                                    tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_cat(res, 401, {{"error", "unauthorized"}}); return; }

    std::string cat_id = req.path_params.at("id");

    std::string entity_id = get_category_entity_id(db, cat_id);
    if (entity_id.empty()) {
        send_json_cat(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_cat(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_cat(res, 400, {{"error", "invalid_json"}}); return; }

    if (body.contains("name") && body["name"].is_string()) {
        std::string n = body["name"].get<std::string>();
        auto upd = db.prepare("UPDATE categories SET name=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, n.data(), static_cast<int>(n.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("kind") && body["kind"].is_string()) {
        std::string k = body["kind"].get<std::string>();
        auto upd = db.prepare("UPDATE categories SET kind=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
        upd.step();
    }

    emit_audit_cat(audit_log, *user_id, entity_id, cat_id,
                   "category_updated", "success", json::object());

    auto sel = db.prepare(
        "SELECT id, entity_id, name, kind FROM categories WHERE id = ?;"
    );
    sqlite3_bind_text(sel.get(), 1,
        cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
    if (sel.step() == SQLITE_ROW) {
        send_json_cat(res, 200, category_row_to_json(sel.get()));
    } else {
        send_json_cat(res, 200, json::object());
    }
}

// DELETE /categories/<id>
static void handle_delete_category(const httplib::Request& req,
                                    httplib::Response& res,
                                    Database& db,
                                    tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_cat(res, 401, {{"error", "unauthorized"}}); return; }

    std::string cat_id = req.path_params.at("id");

    std::string entity_id = get_category_entity_id(db, cat_id);
    if (entity_id.empty()) {
        send_json_cat(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_cat(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto del = db.prepare("DELETE FROM categories WHERE id = ?;");
    sqlite3_bind_text(del.get(), 1,
        cat_id.data(), static_cast<int>(cat_id.size()), SQLITE_STATIC);
    del.step();

    emit_audit_cat(audit_log, *user_id, entity_id, cat_id,
                   "category_deleted", "success", json::object());

    send_json_cat(res, 200, json::object());
}

void register_categories_handlers(httplib::SSLServer& server,
                                   Database& db,
                                   tf::audit::IAuditLog& audit_log)
{
    server.Get("/entities/:entity_id/categories",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_list_categories(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Get("/categories/:id",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_get_category(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Post("/entities/:entity_id/categories",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_create_category(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Put("/categories/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_update_category(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Delete("/categories/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_delete_category(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });
}

} // namespace tf::data
