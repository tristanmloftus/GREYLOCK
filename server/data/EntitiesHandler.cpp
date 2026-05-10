// EntitiesHandler.cpp — /entities CRUD route implementations (Phase 4.B).
//
// GUARDRAILS:
//   F-2: every route calls require_session(); every entity-scoped route calls
//        user_has_access_to_entity() — user-supplied entity_id is never trusted.
//   F-3: every handler wrapped in try/catch.

#include "httplib.h"

#include "EntitiesHandler.h"
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static int64_t now_unix() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static int64_t now_ms() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string generate_id() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

static void emit_audit(tf::audit::IAuditLog& log,
                        const std::string& actor_user_id,
                        const std::string& domain,
                        const std::string& subject_id,
                        const std::string& subject_kind,
                        const std::string& action,
                        const std::string& outcome,
                        const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = now_ms();
    evt.actor_user_id = actor_user_id;
    evt.actor_kind    = "user";
    evt.domain        = domain;
    evt.subject_id    = subject_id;
    evt.subject_kind  = subject_kind;
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;
    log.record(evt);
}

// Validate entity 'kind' is one of the allowed values.
static bool valid_entity_kind(const std::string& kind) {
    return kind == "Individual" || kind == "LLC" || kind == "Corporation"
        || kind == "Partnership" || kind == "Trust" || kind == "Other";
}

// Serialize an entity row from a prepared statement (columns: id, name, kind, created_at_unix).
static json entity_row_to_json(sqlite3_stmt* stmt) {
    json obj;
    const char* id   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    int64_t  created = sqlite3_column_int64(stmt, 3);
    obj["id"]             = id   ? id   : "";
    obj["name"]           = name ? name : "";
    obj["kind"]           = kind ? kind : "";
    obj["created_at_unix"] = created;
    return obj;
}

// ---------------------------------------------------------------------------
// GET /entities — list entities the user is a member of
// ---------------------------------------------------------------------------
static void handle_get_entities(const httplib::Request& req,
                                 httplib::Response& res,
                                 Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT e.id, e.name, e.kind, e.created_at_unix "
        "FROM entities e "
        "JOIN entity_memberships m ON m.entity_id = e.id "
        "WHERE m.user_id = ? "
        "ORDER BY e.created_at_unix ASC;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        user_id->data(), static_cast<int>(user_id->size()), SQLITE_STATIC);

    json items = json::array();
    while (stmt.step() == SQLITE_ROW) {
        items.push_back(entity_row_to_json(stmt.get()));
    }

    send_json(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

// ---------------------------------------------------------------------------
// GET /entities/<id>
// ---------------------------------------------------------------------------
static void handle_get_entity(const httplib::Request& req,
                               httplib::Response& res,
                               Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, name, kind, created_at_unix FROM entities WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json(res, 404, {{"error", "not_found"}});
        return;
    }

    send_json(res, 200, entity_row_to_json(stmt.get()));
}

// ---------------------------------------------------------------------------
// POST /entities — create entity + owner membership
// ---------------------------------------------------------------------------
static void handle_create_entity(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json(res, 401, {{"error", "unauthorized"}}); return; }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json(res, 400, {{"error", "invalid_json"}}); return; }

    std::string name, kind;
    try {
        name = body.at("name").get<std::string>();
        kind = body.at("kind").get<std::string>();
    } catch (...) {
        send_json(res, 400, {{"error", "missing_fields"},
                              {"message", "name, kind required"}});
        return;
    }

    if (!valid_entity_kind(kind)) {
        send_json(res, 400, {{"error", "invalid_kind"},
                              {"message", "kind must be one of: Individual, LLC, Corporation, Partnership, Trust, Other"}});
        return;
    }

    std::string entity_id = generate_id();
    int64_t t = now_unix();

    db.exec("BEGIN IMMEDIATE;");
    try {
        {
            auto ins = db.prepare(
                "INSERT INTO entities (id, name, kind, created_at_unix) "
                "VALUES (?, ?, ?, ?);"
            );
            sqlite3_bind_text(ins.get(), 1, entity_id.data(),
                static_cast<int>(entity_id.size()), SQLITE_STATIC);
            sqlite3_bind_text(ins.get(), 2, name.data(),
                static_cast<int>(name.size()), SQLITE_STATIC);
            sqlite3_bind_text(ins.get(), 3, kind.data(),
                static_cast<int>(kind.size()), SQLITE_STATIC);
            sqlite3_bind_int64(ins.get(), 4, t);
            if (ins.step() != SQLITE_DONE) {
                db.exec("ROLLBACK;");
                send_json(res, 500, {{"error", "internal"}});
                return;
            }
        }
        {
            auto ins = db.prepare(
                "INSERT INTO entity_memberships (user_id, entity_id, role) "
                "VALUES (?, ?, 'owner');"
            );
            sqlite3_bind_text(ins.get(), 1,
                user_id->data(), static_cast<int>(user_id->size()), SQLITE_STATIC);
            sqlite3_bind_text(ins.get(), 2,
                entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
            if (ins.step() != SQLITE_DONE) {
                db.exec("ROLLBACK;");
                send_json(res, 500, {{"error", "internal"}});
                return;
            }
        }
        db.exec("COMMIT;");
    } catch (...) {
        char* errmsg = nullptr;
        sqlite3_exec(db.raw(), "ROLLBACK;", nullptr, nullptr, &errmsg);
        sqlite3_free(errmsg);
        throw;
    }

    emit_audit(audit_log, *user_id, entity_id, entity_id, "entity",
               "entity_created", "success", {{"name", name}, {"kind", kind}});

    send_json(res, 201, {
        {"id", entity_id},
        {"name", name},
        {"kind", kind},
        {"created_at_unix", t}
    });
}

// ---------------------------------------------------------------------------
// PUT /entities/<id>
// ---------------------------------------------------------------------------
static void handle_update_entity(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json(res, 400, {{"error", "invalid_json"}}); return; }

    bool has_name = body.contains("name") && body["name"].is_string();
    bool has_kind = body.contains("kind") && body["kind"].is_string();

    if (has_name) {
        std::string n = body["name"].get<std::string>();
        // no-op if empty
    }
    if (has_kind) {
        std::string k = body["kind"].get<std::string>();
        if (!valid_entity_kind(k)) {
            send_json(res, 400, {{"error", "invalid_kind"}});
            return;
        }
    }

    // Check entity exists.
    {
        auto chk = db.prepare("SELECT 1 FROM entities WHERE id = ?;");
        sqlite3_bind_text(chk.get(), 1,
            entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
        if (chk.step() != SQLITE_ROW) {
            send_json(res, 404, {{"error", "not_found"}});
            return;
        }
    }

    std::string name_val = has_name ? body["name"].get<std::string>() : "";
    std::string kind_val = has_kind ? body["kind"].get<std::string>() : "";

    if (has_name && has_kind) {
        auto upd = db.prepare("UPDATE entities SET name = ?, kind = ? WHERE id = ?;");
        sqlite3_bind_text(upd.get(), 1, name_val.data(), static_cast<int>(name_val.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, kind_val.data(), static_cast<int>(kind_val.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 3, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
        upd.step();
    } else if (has_name) {
        auto upd = db.prepare("UPDATE entities SET name = ? WHERE id = ?;");
        sqlite3_bind_text(upd.get(), 1, name_val.data(), static_cast<int>(name_val.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
        upd.step();
    } else if (has_kind) {
        auto upd = db.prepare("UPDATE entities SET kind = ? WHERE id = ?;");
        sqlite3_bind_text(upd.get(), 1, kind_val.data(), static_cast<int>(kind_val.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
        upd.step();
    }

    emit_audit(audit_log, *user_id, entity_id, entity_id, "entity",
               "entity_updated", "success", json::object());

    auto sel = db.prepare("SELECT id, name, kind, created_at_unix FROM entities WHERE id = ?;");
    sqlite3_bind_text(sel.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
    if (sel.step() == SQLITE_ROW) {
        send_json(res, 200, entity_row_to_json(sel.get()));
    } else {
        send_json(res, 200, json::object());
    }
}

// ---------------------------------------------------------------------------
// DELETE /entities/<id>
// ---------------------------------------------------------------------------
static void handle_delete_entity(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("id");

    if (!user_is_owner_of_entity(db, *user_id, entity_id)) {
        send_json(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto del = db.prepare("DELETE FROM entities WHERE id = ?;");
    sqlite3_bind_text(del.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
    del.step();

    emit_audit(audit_log, *user_id, entity_id, entity_id, "entity",
               "entity_deleted", "success", json::object());

    send_json(res, 200, json::object());
}

// ---------------------------------------------------------------------------
// register_entities_handlers
// ---------------------------------------------------------------------------
void register_entities_handlers(httplib::SSLServer& server,
                                 Database& db,
                                 tf::audit::IAuditLog& audit_log)
{
    server.Get("/entities", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_entities(req, res, db); }
        catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                            "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}}).dump(), "application/json");
        }
    });

    server.Get("/entities/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_entity(req, res, db); }
        catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                            "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}}).dump(), "application/json");
        }
    });

    server.Post("/entities", [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
        try { handle_create_entity(req, res, db, audit_log); }
        catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                            "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}}).dump(), "application/json");
        }
    });

    server.Put("/entities/:id", [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
        try { handle_update_entity(req, res, db, audit_log); }
        catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                            "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}}).dump(), "application/json");
        }
    });

    server.Delete("/entities/:id", [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
        try { handle_delete_entity(req, res, db, audit_log); }
        catch (const std::exception& ex) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}, {"message", ex.what()}}).dump(),
                            "application/json");
        } catch (...) {
            res.status = 500;
            res.set_content(json({{"error", "internal"}}).dump(), "application/json");
        }
    });
}

} // namespace tf::data
