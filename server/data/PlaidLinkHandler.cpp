#include "httplib.h"

#include "PlaidLinkHandler.h"
#include "EntityMembership.h"
#include "../auth/SessionMiddleware.h"
#include "../db/Database.h"
#include "../plaid/PlaidApiClient.h"
#include "../plaid/PlaidTokenBroker.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using json = nlohmann::json;

namespace tf::data {

static void send_json_pl(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static std::string get_account_entity_id_pl(Database& db, const std::string& account_id) {
    auto stmt = db.prepare("SELECT entity_id FROM accounts WHERE id = ?;");
    sqlite3_bind_text(stmt.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        const char* eid = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return eid ? eid : "";
    }
    return "";
}

static void handle_link_init(const httplib::Request& req,
                              httplib::Response& res,
                              Database& db,
                              tf::plaid::PlaidApiClient& plaid)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_pl(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("id");

    std::string entity_id = get_account_entity_id_pl(db, account_id);
    if (entity_id.empty()) {
        send_json_pl(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_pl(res, 403, {{"error", "forbidden"}});
        return;
    }

    auto link_token_opt = plaid.link_token_create(entity_id);
    if (!link_token_opt.has_value()) {
        send_json_pl(res, 500, {{"error", "plaid_link_token_failed"}});
        return;
    }

    send_json_pl(res, 200, {{"link_token", *link_token_opt}});
}

static void handle_link_page(const httplib::Request& req,
                              httplib::Response& res)
{
    std::string account_id = req.get_param_value("account_id");
    std::string link_token = req.get_param_value("link_token");

    std::string html = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Link Bank Account - TerminalFinance</title>
<script src="https://cdn.plaid.com/link/v2/stable/link-initialize.js"></script>
<style>
* { box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; background: #0a0a1a; color: #c8c8d0; }
.card { background: #141428; padding: 2.5rem; border-radius: 12px; text-align: center; max-width: 420px; width: 100%; border: 1px solid #2a2a4a; }
h1 { margin: 0 0 0.5rem; font-size: 1.4rem; font-weight: 600; color: #e8e8f0; }
p { margin: 0 0 1.5rem; font-size: 0.9rem; color: #8888a0; }
#status { margin: 1rem 0; font-size: 0.85rem; min-height: 1.2em; color: #8888a0; }
.btn { display: inline-block; padding: 0.75rem 2rem; font-size: 1rem; border: none; border-radius: 6px; background: #2a2a6a; color: #e0e0f0; cursor: pointer; transition: background 0.2s; }
.btn:hover { background: #3a3a8a; }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
.loader { display: inline-block; width: 16px; height: 16px; border: 2px solid #8888a0; border-top-color: #e0e0f0; border-radius: 50%; animation: spin 0.8s linear infinite; margin-right: 6px; vertical-align: middle; }
@keyframes spin { to { transform: rotate(360deg); } }
</style>
</head>
<body>
<div class="card">
<h1>Connect Your Bank</h1>
<p>Securely link your financial institution to TerminalFinance.</p>
<div id="status"></div>
<button id="link-btn" class="btn">Open Plaid Link</button>
</div>
<script>
var linkToken = ")html" + link_token + R"html(";
var accountId = ")html" + account_id + R"html(";
var statusEl = document.getElementById('status');
var linkBtn = document.getElementById('link-btn');

if (!linkToken || !accountId) {
    statusEl.textContent = 'Missing link token or account ID.';
    linkBtn.disabled = true;
} else {
    var handler = Plaid.create({
        token: linkToken,
        onSuccess: function(public_token, metadata) {
            statusEl.innerHTML = '<span class="loader"></span> Exchanging token...';
            linkBtn.disabled = true;
            var xhr = new XMLHttpRequest();
            xhr.open('POST', '/accounts/' + encodeURIComponent(accountId) + '/link-plaid', true);
            xhr.setRequestHeader('Content-Type', 'application/json');
            xhr.onload = function() {
                if (xhr.status === 200) {
                    statusEl.innerHTML = '&#10003; Bank linked successfully! You can close this window.';
                    statusEl.style.color = '#70c070';
                } else {
                    var msg = 'Error';
                    try { var j = JSON.parse(xhr.responseText); if (j.error) msg = j.error; } catch(e) {}
                    statusEl.textContent = 'Error: ' + msg;
                    statusEl.style.color = '#c07070';
                    linkBtn.disabled = false;
                }
            };
            xhr.onerror = function() {
                statusEl.textContent = 'Network error. Please try again.';
                statusEl.style.color = '#c07070';
                linkBtn.disabled = false;
            };
            xhr.send(JSON.stringify({ public_token: public_token }));
        },
        onExit: function(err, metadata) {
            if (err != null) {
                statusEl.textContent = 'Error: ' + (err.error_message || err.error_type || 'unknown');
                statusEl.style.color = '#c07070';
            } else {
                statusEl.textContent = 'Link flow exited. You can close this window.';
                statusEl.style.color = '#8888a0';
            }
            linkBtn.disabled = false;
        },
        onEvent: function(eventName, metadata) {
            console.log('Plaid Link event:', eventName);
        }
    });

    linkBtn.addEventListener('click', function() {
        statusEl.textContent = '';
        handler.open();
    });
}
</script>
</body>
</html>)html";

    res.set_content(html, "text/html");
}

static void handle_link_plaid(const httplib::Request& req,
                               httplib::Response& res,
                               Database& db,
                               tf::plaid::PlaidApiClient& plaid,
                               tf::plaid::PlaidTokenBroker& broker)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_pl(res, 401, {{"error", "unauthorized"}}); return; }

    std::string account_id = req.path_params.at("id");

    std::string entity_id = get_account_entity_id_pl(db, account_id);
    if (entity_id.empty()) {
        send_json_pl(res, 404, {{"error", "not_found"}});
        return;
    }

    if (!user_has_access_to_entity(db, *user_id, entity_id)) {
        send_json_pl(res, 403, {{"error", "forbidden"}});
        return;
    }

    json body;
    try { body = json::parse(req.body); }
    catch (...) { send_json_pl(res, 400, {{"error", "invalid_json"}}); return; }

    std::string public_token;
    try { public_token = body.at("public_token").get<std::string>(); }
    catch (...) { send_json_pl(res, 400, {{"error", "missing_public_token"}}); return; }

    auto access_token_opt = plaid.item_public_token_exchange(public_token);
    if (!access_token_opt.has_value()) {
        send_json_pl(res, 500, {{"error", "plaid_token_exchange_failed"}});
        return;
    }

    broker.store_token(account_id, *access_token_opt);

    auto upd = db.prepare("UPDATE accounts SET is_plaid_linked=1 WHERE id=?;");
    sqlite3_bind_text(upd.get(), 1,
        account_id.data(), static_cast<int>(account_id.size()), SQLITE_STATIC);
    upd.step();

    send_json_pl(res, 200, {{"success", true}});
}

void register_plaid_link_handlers(
    httplib::SSLServer& server,
    Database& db,
    tf::plaid::PlaidApiClient& plaid_client,
    tf::plaid::PlaidTokenBroker& plaid_broker)
{
    server.Post("/accounts/:id/link/init",
        [&db, &plaid_client](const httplib::Request& req, httplib::Response& res) {
            try { handle_link_init(req, res, db, plaid_client); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Get("/link",
        [](const httplib::Request& req, httplib::Response& res) {
            try { handle_link_page(req, res); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(json({{"error","internal"},{"message",ex.what()}}).dump(), "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(), "application/json");
            }
        });

    server.Post("/accounts/:id/link-plaid",
        [&db, &plaid_client, &plaid_broker](const httplib::Request& req, httplib::Response& res) {
            try { handle_link_plaid(req, res, db, plaid_client, plaid_broker); }
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
