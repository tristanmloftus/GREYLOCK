// BudgetsHandler.cpp — /budgets CRUD route implementations (Phase 4.B).
//
// GUARDRAILS:
//   F-2: every route calls require_session(); every entity-scoped route calls
//        user_has_access_to_entity() — user-supplied entity_id never trusted.
//   F-3: every handler wrapped in try/catch.
//
// Requires M003 migration (budgets table):
//   id TEXT PK, entity_id TEXT NOT NULL, category_id TEXT,
//   amount_cents INTEGER NOT NULL DEFAULT 0,
//   period_start_unix INTEGER NOT NULL, period_end_unix INTEGER NOT NULL

#include "httplib.h"

#include "BudgetsHandler.h"
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

static void send_json_bud(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static int64_t now_ms_bud() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string generate_id_bud() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

static void emit_audit_bud(tf::audit::IAuditLog& log,
                            const std::string& actor_user_id,
                            const std::string& domain,
                            const std::string& subject_id,
                            const std::string& action,
                            const std::string& outcome,
                            const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = now_ms_bud();
    evt.actor_user_id = actor_user_id;
    evt.actor_kind    = "user";
    evt.domain        = domain;
    evt.subject_id    = subject_id;
    evt.subject_kind  = "budget";
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;
    log.record(evt);
}

// Columns: id, entity_id, category_id, amount_cents, period_start_unix, period_end_unix
static json budget_row_to_json(sqlite3_stmt* stmt) {
    json obj;
    const char* id         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* entity_id  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* cat_id     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    int64_t amount_cents   = sqlite3_column_int64(stmt, 3);
    int64_t period_start   = sqlite3_column_int64(stmt, 4);
    int64_t period_end     = sqlite3_column_int64(stmt, 5);

    obj["id"]               = id        ? id        : "";
    obj["entity_id"]        = entity_id ? entity_id : "";
    obj["category_id"]      = cat_id    ? json(cat_id) : json(nullptr);
    obj["amount_cents"]     = amount_cents;
    obj["period_start_unix"]= period_start;
    obj["period_end_unix"]  = period_end;
    return obj;
}

static std::string get_budget_entity_id(Database& db, const std::string& budget_id) {
    auto stmt = db.prepare("SELECT entity_id FROM budgets WHERE id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* eid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return eid ? eid : "";
    }
    return "";
}

// GET /entities/<entity_id>/budgets
static void handle_list_budgets(const httplib::Request& req,
                                 httplib::Response& res,
                                 Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_bud(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_bud(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, category_id, amount_cents, period_start_unix, period_end_unix "
        "FROM budgets WHERE entity_id = ? ORDER BY period_start_unix DESC;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    json items = json::array();
    while (stmt.step() == SQLITE_ROW) {
        items.push_back(budget_row_to_json(stmt.get()));
    }

    send_json_bud(res, 200, {{"items", items}});
}

// GET /budgets/<id>
static void handle_get_budget(const httplib::Request& req,
                               httplib::Response& res,
                               Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_bud(res, 401, {{"error", "unauthorized"}}); return; }

    std::string budget_id = req.path_params.at("id");

    std::string entity_id = get_budget_entity_id(db, budget_id);
    if (entity_id.empty()) {
        send_json_bud(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_bud(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, category_id, amount_cents, period_start_unix, period_end_unix "
        "FROM budgets WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json_bud(res, 404, {{"error", "not_found"}});
        return;
    }

    send_json_bud(res, 200, budget_row_to_json(stmt.get()));
}

// POST /entities/<entity_id>/budgets
static void handle_create_budget(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_bud(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_bud(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_bud(res, 400, {{"error", "invalid_json"}}); return; }

    int64_t amount_cents = 0;
    int64_t period_start = 0;
    int64_t period_end   = 0;
    std::string category_id;

    try {
        amount_cents = body.at("amount_cents").get<int64_t>();
        period_start = body.at("period_start_unix").get<int64_t>();
        period_end   = body.at("period_end_unix").get<int64_t>();
    } catch (...) {
        send_json_bud(res, 400, {{"error", "missing_fields"},
                                  {"message", "amount_cents, period_start_unix, period_end_unix required"}});
        return;
    }

    if (body.contains("category_id") && body["category_id"].is_string()) {
        category_id = body["category_id"].get<std::string>();
    }

    std::string budget_id = generate_id_bud();

    auto ins = db.prepare(
        "INSERT INTO budgets (id, entity_id, category_id, amount_cents, "
        "                     period_start_unix, period_end_unix) "
        "VALUES (?, ?, ?, ?, ?, ?);"
    );
    sqlite3_bind_text(ins.get(), 1, budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 2, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
    if (!category_id.empty()) {
        sqlite3_bind_text(ins.get(), 3, category_id.data(), static_cast<int>(category_id.size()), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(ins.get(), 3);
    }
    sqlite3_bind_int64(ins.get(), 4, amount_cents);
    sqlite3_bind_int64(ins.get(), 5, period_start);
    sqlite3_bind_int64(ins.get(), 6, period_end);

    if (ins.step() != SQLITE_DONE) {
        send_json_bud(res, 500, {{"error", "internal"}});
        return;
    }

    emit_audit_bud(audit_log, *user_id, entity_id, budget_id,
                   "budget_created", "success",
                   {{"amount_cents", amount_cents}});

    send_json_bud(res, 201, {
        {"id", budget_id},
        {"entity_id", entity_id},
        {"category_id", category_id.empty() ? json(nullptr) : json(category_id)},
        {"amount_cents", amount_cents},
        {"period_start_unix", period_start},
        {"period_end_unix", period_end}
    });
}

// PUT /budgets/<id>
static void handle_update_budget(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_bud(res, 401, {{"error", "unauthorized"}}); return; }

    std::string budget_id = req.path_params.at("id");

    std::string entity_id = get_budget_entity_id(db, budget_id);
    if (entity_id.empty()) {
        send_json_bud(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_bud(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_bud(res, 400, {{"error", "invalid_json"}}); return; }

    if (body.contains("amount_cents") && body["amount_cents"].is_number_integer()) {
        int64_t a = body["amount_cents"].get<int64_t>();
        auto upd = db.prepare("UPDATE budgets SET amount_cents=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, a);
        sqlite3_bind_text(upd.get(), 2, budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("period_start_unix") && body["period_start_unix"].is_number_integer()) {
        int64_t p = body["period_start_unix"].get<int64_t>();
        auto upd = db.prepare("UPDATE budgets SET period_start_unix=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, p);
        sqlite3_bind_text(upd.get(), 2, budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("period_end_unix") && body["period_end_unix"].is_number_integer()) {
        int64_t p = body["period_end_unix"].get<int64_t>();
        auto upd = db.prepare("UPDATE budgets SET period_end_unix=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, p);
        sqlite3_bind_text(upd.get(), 2, budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("category_id") && body["category_id"].is_string()) {
        std::string c = body["category_id"].get<std::string>();
        auto upd = db.prepare("UPDATE budgets SET category_id=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, c.data(), static_cast<int>(c.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
        upd.step();
    }

    emit_audit_bud(audit_log, *user_id, entity_id, budget_id,
                   "budget_updated", "success", json::object());

    auto sel = db.prepare(
        "SELECT id, entity_id, category_id, amount_cents, period_start_unix, period_end_unix "
        "FROM budgets WHERE id = ?;"
    );
    sqlite3_bind_text(sel.get(), 1,
        budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
    if (sel.step() == SQLITE_ROW) {
        send_json_bud(res, 200, budget_row_to_json(sel.get()));
    } else {
        send_json_bud(res, 200, json::object());
    }
}

// DELETE /budgets/<id>
static void handle_delete_budget(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db,
                                  tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_bud(res, 401, {{"error", "unauthorized"}}); return; }

    std::string budget_id = req.path_params.at("id");

    std::string entity_id = get_budget_entity_id(db, budget_id);
    if (entity_id.empty()) {
        send_json_bud(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_bud(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto del = db.prepare("DELETE FROM budgets WHERE id = ?;");
    sqlite3_bind_text(del.get(), 1,
        budget_id.data(), static_cast<int>(budget_id.size()), SQLITE_STATIC);
    del.step();

    emit_audit_bud(audit_log, *user_id, entity_id, budget_id,
                   "budget_deleted", "success", json::object());

    send_json_bud(res, 200, json::object());
}

void register_budgets_handlers(httplib::SSLServer& server,
                                Database& db,
                                tf::audit::IAuditLog& audit_log)
{
    server.Get("/entities/:entity_id/budgets",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_list_budgets(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Get("/budgets/:id",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_get_budget(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Post("/entities/:entity_id/budgets",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_create_budget(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Put("/budgets/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_update_budget(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Delete("/budgets/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_delete_budget(req, res, db, audit_log); }
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
