#include "DiscoveryService.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {
    using json = nlohmann::json;

    /*! Default location, relative to current working directory.  Phase 5
     *  ships the file in the repo at data/supplier_map.json so production
     *  builds (cwd == project root or dev/) and tests (cwd == build dir,
     *  but with the file copied/symlinked into place — see CMakeLists) can
     *  both find it.  When the file is absent the service falls back to
     *  the hardcoded ruleset so the binary never crashes on boot. */
    constexpr const char* kDefaultSupplierMapPath = "data/supplier_map.json";

    std::string to_upper(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return out;
    }

    std::string match_kind_to_string(SupplierMatchKind k) {
        switch (k) {
            case SupplierMatchKind::Prefix:   return "prefix";
            case SupplierMatchKind::Contains: return "contains";
        }
        return "contains";
    }

    SupplierMatchKind parse_match_kind(const std::string& s) {
        if (s == "prefix") return SupplierMatchKind::Prefix;
        return SupplierMatchKind::Contains;
    }

    std::string month_from_offset(int offset_months) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif

        tm.tm_mon += offset_months;
        if (tm.tm_mon < 0) { tm.tm_mon += 12; tm.tm_year -= 1; }
        if (tm.tm_mon > 11) { tm.tm_mon -= 12; tm.tm_year += 1; }

        std::ostringstream oss;
        oss << (tm.tm_year + 1900) << "-" << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);
        return oss.str();
    }
}

DiscoveryService& DiscoveryService::instance() {
    static DiscoveryService instance;
    return instance;
}

DiscoveryService::DiscoveryService() {
    // Best-effort JSON load on construction; falls back to the hardcoded
    // ruleset when the file is missing or unparseable so the application
    // (and tests run from an arbitrary CWD) never crash here.
    if (!load_from_json(kDefaultSupplierMapPath)) {
        Logger::instance().warning(
            std::string("DiscoveryService: could not load ") + kDefaultSupplierMapPath
            + " — falling back to hardcoded supplier rules");
        initializeSupplierMap();
    }
}

void DiscoveryService::initializeSupplierMap() {
    // ORDERING MATTERS: rules are evaluated in declaration order, first match
    // wins.  More-specific tokens (e.g. "AMZN MKTPLACE") must precede
    // less-specific ones ("AMAZON") so that ambiguous merchant strings
    // resolve deterministically.
    rules_ = {
        {"AMAZON WEB SERVICES", SupplierMatchKind::Contains, "Amazon Web Services", "AMZN"},
        {"AWS",                 SupplierMatchKind::Contains, "Amazon Web Services", "AMZN"},
        {"AMZN MKTPLACE",       SupplierMatchKind::Contains, "Amazon.com Inc",       "AMZN"},
        {"AMAZON.COM",          SupplierMatchKind::Contains, "Amazon.com Inc",       "AMZN"},
        {"AMAZON",              SupplierMatchKind::Contains, "Amazon.com Inc",       "AMZN"},
        {"AMZN",                SupplierMatchKind::Contains, "Amazon.com Inc",       "AMZN"},
        {"WHOLE FOODS",         SupplierMatchKind::Contains, "Whole Foods (Amazon)", "AMZN"},
        {"GOOGLE CLOUD",        SupplierMatchKind::Contains, "Google Cloud Platform","GOOGL"},
        {"GCP",                 SupplierMatchKind::Contains, "Google Cloud Platform","GOOGL"},
        {"MICROSOFT AZURE",     SupplierMatchKind::Contains, "Microsoft Azure",      "MSFT"},
        {"AZURE",               SupplierMatchKind::Contains, "Microsoft Azure",      "MSFT"},
        {"MICROSOFT",           SupplierMatchKind::Contains, "Microsoft",            "MSFT"},
        {"APPLE.COM",           SupplierMatchKind::Contains, "Apple Inc.",           "AAPL"},
        {"APPLE",               SupplierMatchKind::Contains, "Apple Inc.",           "AAPL"},
        {"NETFLIX",             SupplierMatchKind::Contains, "Netflix",              "NFLX"},
        {"STARBUCKS",           SupplierMatchKind::Contains, "Starbucks Corp",       "SBUX"},
        {"MCDONALD",            SupplierMatchKind::Contains, "McDonald's",           "MCD"},
        {"WALMART",             SupplierMatchKind::Contains, "Walmart",              "WMT"},
        {"COSTCO",              SupplierMatchKind::Contains, "Costco",               "COST"},
        {"HOMEDEPOT.COM",       SupplierMatchKind::Contains, "Home Depot",           "HD"},
        {"HOME DEPOT",          SupplierMatchKind::Contains, "Home Depot",           "HD"},
        {"LOWES",               SupplierMatchKind::Contains, "Lowe's",               "LOW"},
        {"BEST BUY",            SupplierMatchKind::Contains, "Best Buy",             "BBY"},
        {"SPOTIFY",             SupplierMatchKind::Contains, "Spotify",              "SPOT"},
        {"HULU",                SupplierMatchKind::Contains, "Hulu (Disney)",        "DIS"},
        {"DISNEY",              SupplierMatchKind::Contains, "Disney",               "DIS"},
        {"UBER",                SupplierMatchKind::Contains, "Uber",                 "UBER"},
        {"LYFT",                SupplierMatchKind::Contains, "Lyft",                 "LYFT"},
        {"DOORDASH",            SupplierMatchKind::Contains, "DoorDash",             "DASH"},
        {"GRUBHUB",             SupplierMatchKind::Contains, "Grubhub",              "GRUB"},
        {"EXPEDIA",             SupplierMatchKind::Contains, "Expedia",              "EXPE"},
        {"AIRBNB",              SupplierMatchKind::Contains, "Airbnb",               "ABNB"},
        {"MARRIOTT",            SupplierMatchKind::Contains, "Marriott",             "MAR"},
        {"HILTON",              SupplierMatchKind::Contains, "Hilton",               "HLT"},
        {"AT&T",                SupplierMatchKind::Contains, "AT&T",                 "T"},
        {"VERIZON",             SupplierMatchKind::Contains, "Verizon",              "VZ"},
        {"T-MOBILE",            SupplierMatchKind::Contains, "T-Mobile",             "TMUS"},
        {"COMCAST",             SupplierMatchKind::Contains, "Comcast",              "CMCSA"},
        {"CHIPOTLE",            SupplierMatchKind::Contains, "Chipotle",             "CMG"},
        {"PANERA",              SupplierMatchKind::Contains, "Panera",               "PNRA"},
        {"MORTGAGE",            SupplierMatchKind::Contains, "Wells Fargo Mortgage", "WFC"},
        {"CHASE",               SupplierMatchKind::Contains, "JPMorgan Chase",       "JPM"},
        {"BANK OF AMERICA",     SupplierMatchKind::Contains, "Bank of America",      "BAC"},
        {"CITIBANK",            SupplierMatchKind::Contains, "Citibank",             "C"},
        {"GOLDMAN",             SupplierMatchKind::Contains, "Goldman Sachs",        "GS"},
        {"VENMO",               SupplierMatchKind::Contains, "Venmo (PayPal)",       "PYPL"},
        {"PAYPAL",              SupplierMatchKind::Contains, "PayPal",               "PYPL"},
        {"ZOOM",                SupplierMatchKind::Contains, "Zoom",                 "ZM"},
        {"ADOBE",               SupplierMatchKind::Contains, "Adobe",                "ADBE"},
        {"SALESFORCE",          SupplierMatchKind::Contains, "Salesforce",           "CRM"},
        {"ORACLE",              SupplierMatchKind::Contains, "Oracle",               "ORCL"},
        {"IBM",                 SupplierMatchKind::Contains, "IBM",                  "IBM"},
        {"INTEL",               SupplierMatchKind::Contains, "Intel",                "INTC"},
        {"AMD",                 SupplierMatchKind::Contains, "AMD",                  "AMD"},
        {"NVIDIA",              SupplierMatchKind::Contains, "NVIDIA",               "NVDA"},
        {"TESLA",               SupplierMatchKind::Contains, "Tesla",                "TSLA"},
        // Realistic merchant strings the Phase-5 plan calls out explicitly:
        {"TGT ",                SupplierMatchKind::Contains, "Target",               "TGT"},
        {"TARGET",              SupplierMatchKind::Contains, "Target",               "TGT"},
        {"USPS",                SupplierMatchKind::Contains, "USPS",                 ""},
    };

    Logger::instance().info("DiscoveryService: Supplier map initialized (hardcoded fallback) with "
        + std::to_string(rules_.size()) + " entries");
}

bool DiscoveryService::load_from_json(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("DiscoveryService: failed to parse ")
            + path + " — " + ex.what());
        return false;
    }

    if (!doc.is_object() || !doc.contains("rules") || !doc["rules"].is_array()) {
        Logger::instance().error("DiscoveryService: " + path
            + " missing required 'rules' array");
        return false;
    }

    std::vector<SupplierRule> parsed;
    parsed.reserve(doc["rules"].size());
    for (const auto& r : doc["rules"]) {
        if (!r.is_object()) continue;
        SupplierRule rule;
        try {
            rule.match    = to_upper(r.at("match").get<std::string>());
            rule.supplier = r.at("supplier").get<std::string>();
            // ticker may be null/empty/absent for entities w/o a public listing.
            if (r.contains("ticker") && r["ticker"].is_string()) {
                rule.ticker = r["ticker"].get<std::string>();
            }
            std::string mk = r.value("match_kind", std::string("contains"));
            rule.match_kind = parse_match_kind(mk);
        } catch (const std::exception& ex) {
            Logger::instance().error(std::string("DiscoveryService: rule in ")
                + path + " missing required field — " + ex.what());
            return false;
        }
        parsed.push_back(std::move(rule));
    }

    if (parsed.empty()) {
        Logger::instance().error("DiscoveryService: " + path + " contained zero rules");
        return false;
    }

    rules_ = std::move(parsed);
    Logger::instance().info("DiscoveryService: loaded " + std::to_string(rules_.size())
        + " supplier rules from " + path);
    return true;
}

std::string DiscoveryService::get_canonical_mapping_json() const {
    json out;
    out["version"] = 1;
    json arr = json::array();
    for (const auto& r : rules_) {
        json obj;
        obj["match"]      = r.match;
        obj["match_kind"] = match_kind_to_string(r.match_kind);
        obj["supplier"]   = r.supplier;
        obj["ticker"]     = r.ticker;
        arr.push_back(std::move(obj));
    }
    out["rules"] = std::move(arr);
    return out.dump();
}

std::optional<SupplierRule> DiscoveryService::getSupplierInfo(const std::string& description) const {
    if (description.empty()) return std::nullopt;

    std::string upper_desc = to_upper(description);

    for (const auto& rule : rules_) {
        bool hit = false;
        switch (rule.match_kind) {
            case SupplierMatchKind::Contains:
                hit = upper_desc.find(rule.match) != std::string::npos;
                break;
            case SupplierMatchKind::Prefix:
                hit = upper_desc.size() >= rule.match.size()
                   && upper_desc.compare(0, rule.match.size(), rule.match) == 0;
                break;
        }
        if (hit) {
            Logger::instance().debug("DiscoveryService: matched '" + rule.match
                + "' for description: " + description);
            return rule;
        }
    }
    return std::nullopt;
}

std::optional<std::string> DiscoveryService::mapToSupplier(const std::string& description) const {
    auto rule = getSupplierInfo(description);
    if (!rule) return std::nullopt;
    return rule->ticker;
}

std::vector<VelocityResult> DiscoveryService::calculateVelocityForMonths(
    const std::vector<Transaction>& transactions,
    const std::vector<Category>& /*categories*/,
    const std::string& current_month_prefix,
    const std::string& previous_month_prefix
) const {
    std::map<std::string, double> current_month;
    std::map<std::string, double> previous_month;

    for (const auto& tx : transactions) {
        if (tx.amount >= 0) continue;
        if (tx.date.size() < 7) continue;

        const std::string month = tx.date.substr(0, 7);
        if (month == current_month_prefix) {
            current_month[tx.category_id] += std::abs(tx.amount);
        } else if (month == previous_month_prefix) {
            previous_month[tx.category_id] += std::abs(tx.amount);
        }
    }

    // Union of category IDs that appear in either month.  v0.1 only emitted
    // entries that had spend in the CURRENT month, which silently dropped
    // categories that went from non-zero to zero.  The Phase-5 test suite
    // explicitly checks the zero-current-month case, so we include it.
    std::vector<VelocityResult> results;
    std::map<std::string, std::pair<double,double>> merged;
    for (const auto& [cid, v] : current_month)  merged[cid].first  = v;
    for (const auto& [cid, v] : previous_month) merged[cid].second = v;

    for (const auto& [cid, pair] : merged) {
        VelocityResult vr;
        vr.category_id          = cid;
        vr.current_month_spend  = pair.first;
        vr.previous_month_spend = pair.second;
        if (pair.second > 0.0) {
            vr.percent_change =
                ((pair.first - pair.second) / pair.second) * 100.0;
        } else if (pair.first > 0.0) {
            // First-month sentinel: 100% growth from a zero baseline.
            vr.percent_change = 100.0;
        } else {
            vr.percent_change = 0.0;
        }
        results.push_back(vr);
    }

    return results;
}

std::vector<VelocityResult> DiscoveryService::calculateVelocity(
    const std::vector<Transaction>& transactions,
    const std::vector<Category>& categories
) const {
    const std::string current_prefix  = month_from_offset(0);
    const std::string previous_prefix = month_from_offset(-1);
    auto results = calculateVelocityForMonths(
        transactions, categories, current_prefix, previous_prefix);

    Logger::instance().debug("DiscoveryService: Calculated velocity for "
        + std::to_string(results.size()) + " categories");
    return results;
}
