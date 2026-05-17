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

// tasks: id, title, body_md, entity_scope, status, assignee_user_id,
//        due_unix, completed_at_unix, source_note_id, created_at_unix
json task_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]              = T(0);
    o["title"]           = T(1);
    o["body_md"]         = T(2);
    o["entity_scope"]    = T(3);
    o["status"]          = T(4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL) o["assignee_user_id"]  = T(5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["due_unix"]          = sqlite3_column_int64(s, 6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL) o["completed_at_unix"] = sqlite3_column_int64(s, 7);
    if (sqlite3_column_type(s, 8) != SQLITE_NULL) o["source_note_id"]    = T(8);
    o["created_at_unix"] = sqlite3_column_int64(s, 9);
    return o;
}

// events: id, title, body_md, entity_scope, starts_at_unix, ends_at_unix,
//         location, source_note_id, created_at_unix
json event_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]              = T(0);
    o["title"]           = T(1);
    o["body_md"]         = T(2);
    o["entity_scope"]    = T(3);
    o["starts_at_unix"]  = sqlite3_column_int64(s, 4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL) o["ends_at_unix"]    = sqlite3_column_int64(s, 5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["location"]        = T(6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL) o["source_note_id"]  = T(7);
    o["created_at_unix"] = sqlite3_column_int64(s, 8);
    return o;
}

// notes: id, path, title, kind, author_user_id, entity_scope,
//        body_hash_sha256, commit_sha, word_count, last_indexed_at_unix,
//        created_at_unix, updated_at_unix
json note_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]              = T(0);
    o["path"]            = T(1);
    if (sqlite3_column_type(s, 2) != SQLITE_NULL) o["title"] = T(2);
    o["kind"]            = T(3);
    o["author_user_id"]  = T(4);
    o["entity_scope"]    = T(5);
    o["body_hash_sha256"]= T(6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL) o["commit_sha"]            = T(7);
    if (sqlite3_column_type(s, 8) != SQLITE_NULL) o["word_count"]            = sqlite3_column_int64(s, 8);
    if (sqlite3_column_type(s, 9) != SQLITE_NULL) o["last_indexed_at_unix"]  = sqlite3_column_int64(s, 9);
    o["created_at_unix"] = sqlite3_column_int64(s, 10);
    o["updated_at_unix"] = sqlite3_column_int64(s, 11);
    return o;
}

// real_estate: id, label, address, entity_scope, status, acquired_at_unix,
//              cost_basis_cents, current_value_cents, notes_md,
//              created_at_unix, updated_at_unix
json real_estate_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]              = T(0);
    o["label"]           = T(1);
    if (sqlite3_column_type(s, 2) != SQLITE_NULL) o["address"] = T(2);
    o["entity_scope"]    = T(3);
    o["status"]          = T(4);
    if (sqlite3_column_type(s, 5) != SQLITE_NULL) o["acquired_at_unix"]    = sqlite3_column_int64(s, 5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["cost_basis_cents"]    = sqlite3_column_int64(s, 6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL) o["current_value_cents"] = sqlite3_column_int64(s, 7);
    o["notes_md"]        = T(8);
    o["created_at_unix"] = sqlite3_column_int64(s, 9);
    o["updated_at_unix"] = sqlite3_column_int64(s, 10);
    return o;
}

// pending_extractions: id, source_note_id, inference_id, proposed_type,
//                      proposed_payload_json, status, resolved_by_user_id,
//                      resolved_at_unix, resulting_object_id, created_at_unix
json pending_extraction_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]                    = T(0);
    o["source_note_id"]        = T(1);
    o["inference_id"]          = T(2);
    o["proposed_type"]         = T(3);
    o["proposed_payload_json"] = T(4);
    o["status"]                = T(5);
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) o["resolved_by_user_id"]  = T(6);
    if (sqlite3_column_type(s, 7) != SQLITE_NULL) o["resolved_at_unix"]     = sqlite3_column_int64(s, 7);
    if (sqlite3_column_type(s, 8) != SQLITE_NULL) o["resulting_object_id"]  = T(8);
    o["created_at_unix"]       = sqlite3_column_int64(s, 9);
    return o;
}

// reimbursements: id, source_tx_id, owed_by_entity_id, owed_to_entity_id,
//                 amount_cents, status, note, created_at_unix,
//                 resolved_at_unix
// Joined with transactions.description (already encrypted but we render
// the encrypted blob as a hex preview for now; client decrypts later)
// and entities.name for from/to readability.
json reimbursement_row_to_json(sqlite3_stmt* s) {
    json o;
    auto T = [&](int i) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return p ? std::string(p) : std::string();
    };
    o["id"]                   = T(0);
    o["source_tx_id"]         = T(1);
    o["owed_by_entity_id"]    = T(2);
    o["owed_to_entity_id"]    = T(3);
    o["amount_cents"]         = sqlite3_column_int64(s, 4);
    o["status"]               = T(5);
    o["note"]                 = T(6);
    o["created_at_unix"]      = sqlite3_column_int64(s, 7);
    if (sqlite3_column_type(s, 8) != SQLITE_NULL) o["resolved_at_unix"] = sqlite3_column_int64(s, 8);
    // Joined columns (entity names) appended via the SELECT below.
    if (sqlite3_column_count(s) >= 11) {
        const char* a = reinterpret_cast<const char*>(sqlite3_column_text(s, 9));
        const char* b = reinterpret_cast<const char*>(sqlite3_column_text(s, 10));
        if (a) o["owed_by_name"] = a;
        if (b) o["owed_to_name"] = b;
    }
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

    // Exact id or title match first.
    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, outcome, "
        "       decided_by_user_id, decided_at_unix, outcome_due_unix, "
        "       source_note_id, created_at_unix "
        "FROM decisions WHERE id = ? OR title = ? LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);

    if (stmt.step() == SQLITE_ROW) {
        send_json_v3(res, 200, decision_row_to_json(stmt.get()));
        return;
    }

    // Slug fallback identical to relationships — score-based walk so
    // `/decisions/services-arm` matches a row titled "build the services
    // arm slowly".  Score: 3 = slug exact, 2 = any input-token appears as
    // a token in the title slug (split on '-'), 1 = title contains the
    // input as a case-insensitive substring or vice-versa.
    std::string input_slug = slug(id);
    std::string input_lower = lower(id);
    auto stmt2 = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, outcome, "
        "       decided_by_user_id, decided_at_unix, outcome_due_unix, "
        "       source_note_id, created_at_unix "
        "FROM decisions;");
    json best_match;
    int  best_score = 0;
    while (stmt2.step() == SQLITE_ROW) {
        const char* title_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt2.get(), 1));
        if (!title_c) continue;
        std::string title = title_c;
        std::string title_slug = slug(title);
        std::string title_lower = lower(title);

        int score = 0;
        if (title_slug == input_slug) {
            score = 3;
        } else {
            // Check if every input token appears in the title's token set.
            // For multi-word inputs ("services-arm" → "services" + "arm"),
            // require all tokens present; that's stronger than any-token.
            auto tokenize = [](const std::string& s) {
                std::vector<std::string> toks;
                std::string cur;
                for (char c : s) {
                    if (c == '-') {
                        if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                    } else {
                        cur += c;
                    }
                }
                if (!cur.empty()) toks.push_back(cur);
                return toks;
            };
            auto input_toks = tokenize(input_slug);
            auto title_toks = tokenize(title_slug);
            int matched = 0;
            for (const auto& it : input_toks) {
                for (const auto& tt : title_toks) {
                    if (it == tt) { ++matched; break; }
                }
            }
            if (!input_toks.empty() && matched == (int)input_toks.size()) {
                score = 2;
            } else if (!input_lower.empty()
                       && title_lower.find(input_lower) != std::string::npos) {
                score = 1;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_match = decision_row_to_json(stmt2.get());
            if (score == 3) break;
        }
    }
    if (best_score > 0 && !best_match.is_null()) {
        send_json_v3(res, 200, best_match);
        return;
    }
    send_json_v3(res, 404, {{"error", "not_found"}});
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

// ---- tasks ----

void handle_list_tasks(const httplib::Request& req,
                       httplib::Response& res,
                       Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    // Open first (status='open' ranked 0, otherwise 1), then by due_unix
    // (nulls last), then newest created_at_unix.
    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, "
        "       assignee_user_id, due_unix, completed_at_unix, "
        "       source_note_id, created_at_unix "
        "FROM tasks "
        "ORDER BY (status = 'open') DESC, "
        "         (due_unix IS NULL), due_unix ASC, "
        "         created_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(task_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_task(const httplib::Request& req,
                     httplib::Response& res,
                     Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, status, "
        "       assignee_user_id, due_unix, completed_at_unix, "
        "       source_note_id, created_at_unix "
        "FROM tasks WHERE id = ? OR LOWER(title) = LOWER(?) LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, task_row_to_json(stmt.get()));
}

// ---- events ----

void handle_list_events(const httplib::Request& req,
                        httplib::Response& res,
                        Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, starts_at_unix, "
        "       ends_at_unix, location, source_note_id, created_at_unix "
        "FROM events ORDER BY starts_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(event_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_event(const httplib::Request& req,
                      httplib::Response& res,
                      Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    auto stmt = db.prepare(
        "SELECT id, title, body_md, entity_scope, starts_at_unix, "
        "       ends_at_unix, location, source_note_id, created_at_unix "
        "FROM events WHERE id = ? OR LOWER(title) = LOWER(?) LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, event_row_to_json(stmt.get()));
}

// ---- notes ----

void handle_list_notes(const httplib::Request& req,
                       httplib::Response& res,
                       Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, path, title, kind, author_user_id, entity_scope, "
        "       body_hash_sha256, commit_sha, word_count, "
        "       last_indexed_at_unix, created_at_unix, updated_at_unix "
        "FROM notes ORDER BY updated_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(note_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_note(const httplib::Request& req,
                     httplib::Response& res,
                     Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    auto stmt = db.prepare(
        "SELECT id, path, title, kind, author_user_id, entity_scope, "
        "       body_hash_sha256, commit_sha, word_count, "
        "       last_indexed_at_unix, created_at_unix, updated_at_unix "
        "FROM notes WHERE id = ? OR path = ? LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, note_row_to_json(stmt.get()));
}

// ---- real_estate ----

void handle_list_real_estate(const httplib::Request& req,
                             httplib::Response& res,
                             Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, label, address, entity_scope, status, "
        "       acquired_at_unix, cost_basis_cents, current_value_cents, "
        "       notes_md, created_at_unix, updated_at_unix "
        "FROM real_estate ORDER BY label ASC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW) items.push_back(real_estate_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_real_estate(const httplib::Request& req,
                            httplib::Response& res,
                            Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    std::string sl = slug(id);

    auto stmt = db.prepare(
        "SELECT id, label, address, entity_scope, status, "
        "       acquired_at_unix, cost_basis_cents, current_value_cents, "
        "       notes_md, created_at_unix, updated_at_unix "
        "FROM real_estate "
        "WHERE id = ? OR LOWER(label) = LOWER(?) "
        "ORDER BY id LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() == SQLITE_ROW) {
        send_json_v3(res, 200, real_estate_row_to_json(stmt.get()));
        return;
    }

    // Slug fallback identical to relationships: substring match on label.
    auto stmt2 = db.prepare(
        "SELECT id, label, address, entity_scope, status, "
        "       acquired_at_unix, cost_basis_cents, current_value_cents, "
        "       notes_md, created_at_unix, updated_at_unix "
        "FROM real_estate;");
    json best_match;
    int  best_score = 0;
    std::string lowered_id = lower(id);
    while (stmt2.step() == SQLITE_ROW) {
        const char* lb_c = reinterpret_cast<const char*>(sqlite3_column_text(stmt2.get(), 1));
        if (!lb_c) continue;
        std::string lb  = lb_c;
        std::string lbs = slug(lb);

        int score = 0;
        if (lbs == sl) score = 3;
        else if (!sl.empty() && lower(lb).find(lowered_id) != std::string::npos) score = 1;
        if (score > best_score) {
            best_score = score;
            best_match = real_estate_row_to_json(stmt2.get());
            if (score == 3) break;
        }
    }
    if (best_score > 0 && !best_match.is_null()) {
        send_json_v3(res, 200, best_match);
        return;
    }
    send_json_v3(res, 404, {{"error", "not_found"}});
}

// ---- pending_extractions ----

void handle_list_pending_extractions(const httplib::Request& req,
                                     httplib::Response& res,
                                     Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    auto stmt = db.prepare(
        "SELECT id, source_note_id, inference_id, proposed_type, "
        "       proposed_payload_json, status, resolved_by_user_id, "
        "       resolved_at_unix, resulting_object_id, created_at_unix "
        "FROM pending_extractions "
        "ORDER BY (status = 'pending') DESC, created_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW)
        items.push_back(pending_extraction_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_pending_extraction(const httplib::Request& req,
                                   httplib::Response& res,
                                   Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    auto stmt = db.prepare(
        "SELECT id, source_note_id, inference_id, proposed_type, "
        "       proposed_payload_json, status, resolved_by_user_id, "
        "       resolved_at_unix, resulting_object_id, created_at_unix "
        "FROM pending_extractions WHERE id = ? LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, pending_extraction_row_to_json(stmt.get()));
}

// ---- reimbursements ----

void handle_list_reimbursements(const httplib::Request& req,
                                httplib::Response& res,
                                Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    // Join entities twice for from/to readability.  pending first,
    // then newest created_at_unix.
    auto stmt = db.prepare(
        "SELECT r.id, r.source_tx_id, r.owed_by_entity_id, r.owed_to_entity_id, "
        "       r.amount_cents, r.status, r.note, r.created_at_unix, "
        "       r.resolved_at_unix, eb.name, et.name "
        "FROM reimbursements r "
        "LEFT JOIN entities eb ON eb.id = r.owed_by_entity_id "
        "LEFT JOIN entities et ON et.id = r.owed_to_entity_id "
        "ORDER BY (r.status = 'pending') DESC, r.created_at_unix DESC;");
    json items = json::array();
    while (stmt.step() == SQLITE_ROW)
        items.push_back(reimbursement_row_to_json(stmt.get()));
    send_json_v3(res, 200, {{"items", items}, {"next_cursor", nullptr}});
}

void handle_get_reimbursement(const httplib::Request& req,
                              httplib::Response& res,
                              Database& db)
{
    auto user_id = require_session(req, db);
    if (!user_id) { send_json_v3(res, 401, {{"error", "unauthorized"}}); return; }

    std::string id = req.path_params.at("id");
    auto stmt = db.prepare(
        "SELECT r.id, r.source_tx_id, r.owed_by_entity_id, r.owed_to_entity_id, "
        "       r.amount_cents, r.status, r.note, r.created_at_unix, "
        "       r.resolved_at_unix, eb.name, et.name "
        "FROM reimbursements r "
        "LEFT JOIN entities eb ON eb.id = r.owed_by_entity_id "
        "LEFT JOIN entities et ON et.id = r.owed_to_entity_id "
        "WHERE r.id = ? LIMIT 1;");
    sqlite3_bind_text(stmt.get(), 1, id.data(), (int)id.size(), SQLITE_STATIC);
    if (stmt.step() != SQLITE_ROW) {
        send_json_v3(res, 404, {{"error", "not_found"}});
        return;
    }
    send_json_v3(res, 200, reimbursement_row_to_json(stmt.get()));
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

    server.Get("/tasks",           [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_tasks(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/tasks/:id",       [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_task(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });

    server.Get("/events",          [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_events(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/events/:id",      [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_event(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });

    server.Get("/notes",           [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_notes(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/notes/:id",       [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_note(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });

    server.Get("/real_estate",     [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_real_estate(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/real_estate/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_real_estate(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });

    server.Get("/pending_extractions",     [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_pending_extractions(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/pending_extractions/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_pending_extraction(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });

    server.Get("/reimbursements",     [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_list_reimbursements(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
    server.Get("/reimbursements/:id", [&db](const httplib::Request& req, httplib::Response& res) {
        try { handle_get_reimbursement(req, res, db); }
        catch (const std::exception& ex) { send_json_v3(res, 500, {{"error","internal"},{"message",ex.what()}}); }
    });
}

}  // namespace tf::data
