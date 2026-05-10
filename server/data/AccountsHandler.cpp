// AccountsHandler.cpp — /accounts CRUD route implementations (Phase 4.B).
//
// SECURITY GUARDRAIL: accounts.encrypted_token is NEVER serialized in any response.
//
// GUARDRAILS:
//   F-2: every route calls require_session(); entity access verified via DB.
//   F-3: every handler wrapped in try/catch.

#include "httplib.h"

#include "AccountsHandler.h"
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

static void send_json_acc(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static int64_t now_unix_acc() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static int64_t now_ms_acc() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string generate_id_acc() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

static void emit_audit_acc(tf::audit::IAuditLog& log,
                            const std::string& actor_user_id,
                            const std::string& domain,
                            const std::string& subject_id,
                            const std::string& subject_kind,
                            const std::string& action,
                            const std::string& outcome,
                            const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = now_ms_acc();
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

// Serialize an account row — NEVER includes encrypted_token.
// Columns: id, entity_id, name, kind, balance_cents, plaid_item_id,
//          plaid_account_id, is_plaid_linked, created_at_unix
static json account_row_to_json(sqlite3_stmt* stmt) {
    json obj;
    const char* id        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* entity_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* name      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const char* kind      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    int64_t balance_cents = sqlite3_column_int64(stmt, 4);
    const char* plaid_item  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    const char* plaid_acct  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    int is_plaid_linked     = sqlite3_column_int(stmt, 7);
    int64_t created         = sqlite3_column_int64(stmt, 8);

    obj["id"]              = id        ? id        : "";
    obj["entity_id"]       = entity_id ? entity_id : "";
    obj["name"]            = name      ? name      : "";
    obj["kind"]            = kind      ? kind      : "";
    obj["balance_cents"]   = balance_cents;
    obj["plaid_item_id"]   = plaid_item ? json(plaid_item) : json(nullptr);
    obj["plaid_account_id"]= plaid_acct ? json(plaid_acct) : json(nullptr);
    obj["is_plaid_linked"] = (is_plaid_linked != 0);
    obj["created_at_unix"] = created;
    // IMPORTANT: encrypted_token is deliberately NOT included.
    return obj;
}

static std::string get_account_entity_id(Database& db, const std::string& account_id) {
    auto stmt = db.prepare("SELECT entity_id FROM accounts WHERE id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* eid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return eid ? eid : "";
    }
    return "";
}

// GET /entities/<entity_id>/accounts
static void handle_list_accounts(const httplib::Request& req,
                                  httplib::Response& res,
                                  Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_acc(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_acc(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, name, kind, balance_cents, plaid_item_id, "
        "       plaid_account_id, is_plaid_linked, created_at_unix "
        "FROM accounts WHERE entity_id = ? ORDER BY created_at_unix ASC;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);

    json items = json::array();
    while (stmt.step() == SQLITE_ROW) {
        items.push_back(account_row_to_json(stmt.get()));
    }

    send_json_acc(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

// GET /accounts/<id>
static void handle_get_account(const httplib::Request& req,
                                httplib::Response& res,
                                Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_acc(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("id");

    std::string entity_id = get_account_entity_id(db, account_id);
    if (entity_id.empty()) {
        send_json_acc(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_acc(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, entity_id, name, kind, balance_cents, plaid_item_id, "
        "       plaid_account_id, is_plaid_linked, created_at_unix "
        "FROM accounts WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json_acc(res, 404, {{"error", "not_found"}});
        return;
    }

    send_json_acc(res, 200, account_row_to_json(stmt.get()));
}

// POST /entities/<entity_id>/accounts
static void handle_create_account(const httplib::Request& req,
                                   httplib::Response& res,
                                   Database& db,
                                   tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_acc(res, 401, {{"error", "unauthorized"}}); return; }

    std::string entity_id = req.path_params.at("entity_id");

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_acc(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_acc(res, 400, {{"error", "invalid_json"}}); return; }

    std::string name, kind;
    int64_t balance_cents = 0;
    try {
        name = body.at("name").get<std::string>();
        kind = body.at("kind").get<std::string>();
        if (body.contains("balance_cents") && body["balance_cents"].is_number_integer()) {
            balance_cents = body["balance_cents"].get<int64_t>();
        }
    } catch (...) {
        send_json_acc(res, 400, {{"error", "missing_fields"},
                                  {"message", "name, kind required"}});
        return;
    }

    std::string account_id = generate_id_acc();
    int64_t t = now_unix_acc();

    auto ins = db.prepare(
        "INSERT INTO accounts (id, entity_id, name, kind, balance_cents, "
        "                      is_plaid_linked, created_at_unix) "
        "VALUES (?, ?, ?, ?, ?, 0, ?);"
    );
    sqlite3_bind_text(ins.get(), 1, account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 2, entity_id.data(), static_cast<int>(entity_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 3, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 4, kind.data(), static_cast<int>(kind.size()), SQLITE_STATIC);
    sqlite3_bind_int64(ins.get(), 5, balance_cents);
    sqlite3_bind_int64(ins.get(), 6, t);

    if (ins.step() != SQLITE_DONE) {
        send_json_acc(res, 500, {{"error", "internal"}});
        return;
    }

    emit_audit_acc(audit_log, *user_id, entity_id, account_id, "account",
                   "account_created", "success",
                   {{"name", name}, {"kind", kind}});

    send_json_acc(res, 201, {
        {"id", account_id},
        {"entity_id", entity_id},
        {"name", name},
        {"kind", kind},
        {"balance_cents", balance_cents},
        {"plaid_item_id", nullptr},
        {"plaid_account_id", nullptr},
        {"is_plaid_linked", false},
        {"created_at_unix", t}
    });
}

// PUT /accounts/<id>
static void handle_update_account(const httplib::Request& req,
                                   httplib::Response& res,
                                   Database& db,
                                   tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_acc(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("id");

    std::string entity_id = get_account_entity_id(db, account_id);
    if (entity_id.empty()) {
        send_json_acc(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_acc(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_acc(res, 400, {{"error", "invalid_json"}}); return; }

    bool has_name    = body.contains("name")          && body["name"].is_string();
    bool has_kind    = body.contains("kind")          && body["kind"].is_string();
    bool has_balance = body.contains("balance_cents") && body["balance_cents"].is_number_integer();

    if (has_name) {
        std::string n = body["name"].get<std::string>();
        auto upd = db.prepare("UPDATE accounts SET name=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, n.data(), static_cast<int>(n.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (has_kind) {
        std::string k = body["kind"].get<std::string>();
        auto upd = db.prepare("UPDATE accounts SET kind=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, k.data(), static_cast<int>(k.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (has_balance) {
        int64_t b = body["balance_cents"].get<int64_t>();
        auto upd = db.prepare("UPDATE accounts SET balance_cents=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, b);
        sqlite3_bind_text(upd.get(), 2, account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
        upd.step();
    }

    emit_audit_acc(audit_log, *user_id, entity_id, account_id, "account",
                   "account_updated", "success", json::object());

    auto sel = db.prepare(
        "SELECT id, entity_id, name, kind, balance_cents, plaid_item_id, "
        "       plaid_account_id, is_plaid_linked, created_at_unix "
        "FROM accounts WHERE id = ?;"
    );
    sqlite3_bind_text(sel.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    if (sel.step() == SQLITE_ROW) {
        send_json_acc(res, 200, account_row_to_json(sel.get()));
    } else {
        send_json_acc(res, 200, json::object());
    }
}

// DELETE /accounts/<id>
static void handle_delete_account(const httplib::Request& req,
                                   httplib::Response& res,
                                   Database& db,
                                   tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_acc(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("id");

    std::string entity_id = get_account_entity_id(db, account_id);
    if (entity_id.empty()) {
        send_json_acc(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_acc(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto del = db.prepare("DELETE FROM accounts WHERE id = ?;");
    sqlite3_bind_text(del.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    del.step();

    emit_audit_acc(audit_log, *user_id, entity_id, account_id, "account",
                   "account_deleted", "success", json::object());

    send_json_acc(res, 200, json::object());
}

void register_accounts_handlers(httplib::SSLServer& server,
                                 Database& db,
                                 tf::audit::IAuditLog& audit_log)
{
    server.Get("/entities/:entity_id/accounts",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_list_accounts(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Get("/accounts/:id",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_get_account(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Post("/entities/:entity_id/accounts",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_create_account(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Put("/accounts/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_update_account(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Delete("/accounts/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_delete_account(req, res, db, audit_log); }
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
