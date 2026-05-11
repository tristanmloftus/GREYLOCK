// SupplierMapHandler.cpp — GET /supplier-map (Phase 5).
//
// Validates and re-serialises the on-disk JSON each request.  This is the
// canonical source of truth for the TUI's supplier-recognition rules; we
// intentionally re-parse on each request so that operators editing
// data/supplier_map.json in production get an immediate response (the file
// is small — ~2 KB — and supplier-map fetches are infrequent).
//
// GUARDRAILS:
//   F-2: session-gated via require_session().
//   F-3: every handler body wrapped in try/catch.
//   F-5: no IP/XFF gating — session check uses email-keyed buckets.
//   No leakage: 500 body is the fixed string {"error":"supplier_map_unavailable"}
//   regardless of whether the file is missing, unreadable, or malformed.

#include "httplib.h"

#include "SupplierMapHandler.h"
#include "../auth/SessionMiddleware.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace tf::discovery {

namespace {

void send_json(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Read file -> validate shape -> re-serialise.  Returns std::nullopt
// on any IO or schema failure; the caller emits a 500 with the
// non-leaking error body.
std::optional<std::string> read_and_validate(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string raw = buf.str();
    if (raw.empty()) return std::nullopt;

    json doc;
    try {
        doc = json::parse(raw);
    } catch (...) {
        return std::nullopt;
    }

    if (!doc.is_object() || !doc.contains("rules") || !doc["rules"].is_array()) {
        return std::nullopt;
    }
    // Sanity-check every rule has at minimum `match` and `supplier`.
    for (const auto& r : doc["rules"]) {
        if (!r.is_object()) return std::nullopt;
        if (!r.contains("match")    || !r["match"].is_string())    return std::nullopt;
        if (!r.contains("supplier") || !r["supplier"].is_string()) return std::nullopt;
    }

    // Re-serialise to a canonical form.  Drop any unknown top-level fields
    // (e.g. _doc comments) so the response is strictly the schema.
    json out;
    out["version"] = doc.value("version", 1);
    json arr = json::array();
    for (const auto& r : doc["rules"]) {
        json o;
        o["match"]      = r.at("match").get<std::string>();
        o["match_kind"] = r.value("match_kind", std::string("contains"));
        o["supplier"]   = r.at("supplier").get<std::string>();
        o["ticker"]     = r.value("ticker", std::string(""));
        arr.push_back(std::move(o));
    }
    out["rules"] = std::move(arr);
    return out.dump();
}

void handle_get(const httplib::Request& req,
                httplib::Response& res,
                Database& db,
                const std::string& path)
{
    auto user_id = tf::data::require_session(req, db);
    if (!user_id) {
        send_json(res, 401, {{"error", "unauthorized"}});
        return;
    }

    auto body = read_and_validate(path);
    if (!body) {
        send_json(res, 500, {{"error", "supplier_map_unavailable"}});
        return;
    }

    res.status = 200;
    res.set_content(*body, "application/json");
}

} // namespace

void register_supplier_map_handler(httplib::SSLServer& server,
                                    Database& db,
                                    std::string json_path)
{
    server.Get("/supplier-map",
        [&db, path = std::move(json_path)](const httplib::Request& req,
                                            httplib::Response& res) {
            try { handle_get(req, res, db, path); }
            catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(
                    json({{"error","internal"},{"message",ex.what()}}).dump(),
                    "application/json");
            } catch (...) {
                res.status = 500;
                res.set_content(json({{"error","internal"}}).dump(),
                                "application/json");
            }
        });
}

} // namespace tf::discovery
