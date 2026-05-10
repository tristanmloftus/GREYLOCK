// TransactionsHandler.cpp — /transactions CRUD route implementations (Phase 4.B).
//
// description_encrypted: v0.2 stores description as UTF-8 bytes in BLOB column.
// Phase 4.C TODO: envelope encryption before storing.
//
// Pagination: offset-based. Phase 5+ can upgrade to keyset-based.

#include "httplib.h"

#include "TransactionsHandler.h"
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
#include <limits>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace tf::data {

static constexpr int64_t kDefaultLimit = 100;
static constexpr int64_t kMaxLimit     = 1000;

static void send_json_tx(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static int64_t now_unix_tx() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static int64_t now_ms_tx() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string generate_id_tx() {
    uint8_t bytes[16];
    randombytes_buf(bytes, sizeof(bytes));
    char hex[33];
    sodium_bin2hex(hex, sizeof(hex), bytes, sizeof(bytes));
    return std::string(hex);
}

static void emit_audit_tx(tf::audit::IAuditLog& log,
                           const std::string& actor_user_id,
                           const std::string& domain,
                           const std::string& subject_id,
                           const std::string& action,
                           const std::string& outcome,
                           const json& details = json::object())
{
    tf::audit::AuditEvent evt;
    evt.ts_ms         = now_ms_tx();
    evt.actor_user_id = actor_user_id;
    evt.actor_kind    = "user";
    evt.domain        = domain;
    evt.subject_id    = subject_id;
    evt.subject_kind  = "transaction";
    evt.action        = action;
    evt.outcome       = outcome;
    evt.details       = details;
    log.record(evt);
}

// Deserialize description from BLOB (v0.2: stored as UTF-8 bytes).
static std::string deserialize_description(sqlite3_stmt* stmt, int col) {
    const void* data = sqlite3_column_blob(stmt, col);
    int len          = sqlite3_column_bytes(stmt, col);
    if (!data || len <= 0) return "";
    return std::string(static_cast<const char*>(data), static_cast<size_t>(len));
}

// Columns: id, account_id, plaid_transaction_id, posted_at_unix,
//          amount_cents, description_encrypted (blob), category, created_at_unix
static json tx_row_to_json(sqlite3_stmt* stmt) {
    json obj;
    const char* id        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* acct_id   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* plaid_id  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    int64_t posted        = sqlite3_column_int64(stmt, 3);
    int64_t amount_cents  = sqlite3_column_int64(stmt, 4);
    std::string desc      = deserialize_description(stmt, 5);
    const char* category  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    int64_t created       = sqlite3_column_int64(stmt, 7);

    obj["id"]                   = id      ? id      : "";
    obj["account_id"]           = acct_id ? acct_id : "";
    obj["plaid_transaction_id"] = plaid_id ? json(plaid_id) : json(nullptr);
    obj["posted_at_unix"]       = posted;
    obj["amount_cents"]         = amount_cents;
    obj["description"]          = desc;
    obj["category"]             = category ? json(category) : json(nullptr);
    obj["created_at_unix"]      = created;
    return obj;
}

static std::string get_tx_account_entity_id(Database& db, const std::string& account_id) {
    auto stmt = db.prepare("SELECT entity_id FROM accounts WHERE id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* eid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return eid ? eid : "";
    }
    return "";
}

static std::pair<std::string,std::string> get_tx_ids(Database& db, const std::string& tx_id) {
    auto stmt = db.prepare(
        "SELECT t.account_id, a.entity_id "
        "FROM transactions t JOIN accounts a ON a.id = t.account_id "
        "WHERE t.id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* acct = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const char* ent  = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        return {acct ? acct : "", ent ? ent : ""};
    }
    return {"", ""};
}

static int64_t parse_qparam_int(const httplib::Request& req,
                                 const std::string& key,
                                 int64_t default_val)
{
    if (!req.has_param(key)) return default_val;
    try { return std::stoll(req.get_param_value(key)); }
    catch (...) { return default_val; }
}

// GET /accounts/<account_id>/transactions
static void handle_list_transactions(const httplib::Request& req,
                                      httplib::Response& res,
                                      Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_tx(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("account_id");

    std::string entity_id = get_tx_account_entity_id(db, account_id);
    if (entity_id.empty()) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_tx(res, 403, {{"error", "forbidden"}});
        return;
    }

    int64_t from   = parse_qparam_int(req, "from",   0);
    int64_t to     = parse_qparam_int(req, "to",     std::numeric_limits<int64_t>::max());
    int64_t limit  = parse_qparam_int(req, "limit",  kDefaultLimit);
    int64_t offset = parse_qparam_int(req, "offset", 0);

    if (limit <= 0 || limit > kMaxLimit) limit = kDefaultLimit;
    if (offset < 0) offset = 0;

    auto stmt = db.prepare(
        "SELECT id, account_id, plaid_transaction_id, posted_at_unix, "
        "       amount_cents, description_encrypted, category, created_at_unix "
        "FROM transactions "
        "WHERE account_id = ? AND posted_at_unix >= ? AND posted_at_unix <= ? "
        "ORDER BY posted_at_unix DESC "
        "LIMIT ? OFFSET ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    sqlite3_bind_int64(stmt.get(), 2, from);
    sqlite3_bind_int64(stmt.get(), 3, to);
    sqlite3_bind_int64(stmt.get(), 4, limit);
    sqlite3_bind_int64(stmt.get(), 5, offset);

    json items = json::array();
    while (stmt.step() == SQLITE_ROW) {
        items.push_back(tx_row_to_json(stmt.get()));
    }

    json next_cursor = nullptr;
    if (static_cast<int64_t>(items.size()) == limit) {
        next_cursor = std::to_string(offset + limit);
    }

    send_json_tx(res, 200, {{"items", items}, {"next_cursor", next_cursor}});
}

// GET /transactions/<id>
static void handle_get_transaction(const httplib::Request& req,
                                    httplib::Response& res,
                                    Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_tx(res, 401, {{"error", "unauthorized"}}); return; }

    std::string tx_id = req.path_params.at("id");

    auto [account_id, entity_id] = get_tx_ids(db, tx_id);
    if (entity_id.empty()) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_tx(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto stmt = db.prepare(
        "SELECT id, account_id, plaid_transaction_id, posted_at_unix, "
        "       amount_cents, description_encrypted, category, created_at_unix "
        "FROM transactions WHERE id = ?;"
    );
    sqlite3_bind_text(stmt.get(), 1,
        tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);

    if (stmt.step() != SQLITE_ROW) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    send_json_tx(res, 200, tx_row_to_json(stmt.get()));
}

// POST /accounts/<account_id>/transactions
static void handle_create_transaction(const httplib::Request& req,
                                       httplib::Response& res,
                                       Database& db,
                                       tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_tx(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("account_id");

    std::string entity_id = get_tx_account_entity_id(db, account_id);
    if (entity_id.empty()) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_tx(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_tx(res, 400, {{"error", "invalid_json"}}); return; }

    int64_t amount_cents = 0;
    int64_t posted_at_unix = now_unix_tx();
    std::string description, category;

    try {
        amount_cents = body.at("amount_cents").get<int64_t>();
    } catch (...) {
        send_json_tx(res, 400, {{"error", "missing_fields"},
                                 {"message", "amount_cents required"}});
        return;
    }

    if (body.contains("posted_at_unix") && body["posted_at_unix"].is_number_integer()) {
        posted_at_unix = body["posted_at_unix"].get<int64_t>();
    }
    if (body.contains("description") && body["description"].is_string()) {
        description = body["description"].get<std::string>();
    }
    if (body.contains("category") && body["category"].is_string()) {
        category = body["category"].get<std::string>();
    }

    std::string tx_id = generate_id_tx();
    int64_t t = now_unix_tx();

    // v0.2: store description as raw UTF-8 bytes in description_encrypted BLOB.
    // Phase 4.C TODO: envelope encrypt before storing.
    auto ins = db.prepare(
        "INSERT INTO transactions (id, account_id, posted_at_unix, amount_cents, "
        "                          description_encrypted, category, created_at_unix) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);"
    );
    sqlite3_bind_text(ins.get(), 1, tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(ins.get(), 2, account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    sqlite3_bind_int64(ins.get(), 3, posted_at_unix);
    sqlite3_bind_int64(ins.get(), 4, amount_cents);
    if (!description.empty()) {
        sqlite3_bind_blob(ins.get(), 5,
            description.data(), static_cast<int>(description.size()), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(ins.get(), 5);
    }
    if (!category.empty()) {
        sqlite3_bind_text(ins.get(), 6, category.data(), static_cast<int>(category.size()), SQLITE_STATIC);
    } else {
        sqlite3_bind_null(ins.get(), 6);
    }
    sqlite3_bind_int64(ins.get(), 7, t);

    if (ins.step() != SQLITE_DONE) {
        send_json_tx(res, 500, {{"error", "internal"}});
        return;
    }

    emit_audit_tx(audit_log, *user_id, entity_id, tx_id,
                  "transaction_created", "success",
                  {{"account_id", account_id}, {"amount_cents", amount_cents}});

    send_json_tx(res, 201, {
        {"id", tx_id},
        {"account_id", account_id},
        {"plaid_transaction_id", nullptr},
        {"posted_at_unix", posted_at_unix},
        {"amount_cents", amount_cents},
        {"description", description},
        {"category", category.empty() ? json(nullptr) : json(category)},
        {"created_at_unix", t}
    });
}

// PUT /transactions/<id>
static void handle_update_transaction(const httplib::Request& req,
                                       httplib::Response& res,
                                       Database& db,
                                       tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_tx(res, 401, {{"error", "unauthorized"}}); return; }

    std::string tx_id = req.path_params.at("id");

    auto [account_id, entity_id] = get_tx_ids(db, tx_id);
    if (entity_id.empty()) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_tx(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_tx(res, 400, {{"error", "invalid_json"}}); return; }

    if (body.contains("amount_cents") && body["amount_cents"].is_number_integer()) {
        int64_t a = body["amount_cents"].get<int64_t>();
        auto upd = db.prepare("UPDATE transactions SET amount_cents=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, a);
        sqlite3_bind_text(upd.get(), 2, tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("posted_at_unix") && body["posted_at_unix"].is_number_integer()) {
        int64_t p = body["posted_at_unix"].get<int64_t>();
        auto upd = db.prepare("UPDATE transactions SET posted_at_unix=? WHERE id=?;");
        sqlite3_bind_int64(upd.get(), 1, p);
        sqlite3_bind_text(upd.get(), 2, tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("description") && body["description"].is_string()) {
        std::string d = body["description"].get<std::string>();
        auto upd = db.prepare("UPDATE transactions SET description_encrypted=? WHERE id=?;");
        sqlite3_bind_blob(upd.get(), 1, d.data(), static_cast<int>(d.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
        upd.step();
    }
    if (body.contains("category") && body["category"].is_string()) {
        std::string c = body["category"].get<std::string>();
        auto upd = db.prepare("UPDATE transactions SET category=? WHERE id=?;");
        sqlite3_bind_text(upd.get(), 1, c.data(), static_cast<int>(c.size()), SQLITE_STATIC);
        sqlite3_bind_text(upd.get(), 2, tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
        upd.step();
    }

    emit_audit_tx(audit_log, *user_id, entity_id, tx_id,
                  "transaction_updated", "success", json::object());

    auto sel = db.prepare(
        "SELECT id, account_id, plaid_transaction_id, posted_at_unix, "
        "       amount_cents, description_encrypted, category, created_at_unix "
        "FROM transactions WHERE id = ?;"
    );
    sqlite3_bind_text(sel.get(), 1,
        tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
    if (sel.step() == SQLITE_ROW) {
        send_json_tx(res, 200, tx_row_to_json(sel.get()));
    } else {
        send_json_tx(res, 200, json::object());
    }
}

// DELETE /transactions/<id>
static void handle_delete_transaction(const httplib::Request& req,
                                       httplib::Response& res,
                                       Database& db,
                                       tf::audit::IAuditLog& audit_log)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_tx(res, 401, {{"error", "unauthorized"}}); return; }

    std::string tx_id = req.path_params.at("id");

    auto [account_id, entity_id] = get_tx_ids(db, tx_id);
    if (entity_id.empty()) {
        send_json_tx(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_tx(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto del = db.prepare("DELETE FROM transactions WHERE id = ?;");
    sqlite3_bind_text(del.get(), 1,
        tx_id.data(), static_cast<int>(tx_id.size()), SQLITE_STATIC);
    del.step();

    emit_audit_tx(audit_log, *user_id, entity_id, tx_id,
                  "transaction_deleted", "success", json::object());

    send_json_tx(res, 200, json::object());
}

void register_transactions_handlers(httplib::SSLServer& server,
                                     Database& db,
                                     tf::audit::IAuditLog& audit_log)
{
    server.Get("/accounts/:account_id/transactions",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_list_transactions(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Get("/transactions/:id",
        [&db](const httplib::Request& req, httplib::Response& res) {
            try { handle_get_transaction(req, res, db); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Post("/accounts/:account_id/transactions",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_create_transaction(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Put("/transactions/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_update_transaction(req, res, db, audit_log); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Delete("/transactions/:id",
        [&db, &audit_log](const httplib::Request& req, httplib::Response& res) {
            try { handle_delete_transaction(req, res, db, audit_log); }
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
