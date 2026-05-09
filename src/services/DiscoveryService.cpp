#include "DiscoveryService.h"
#include <chrono>
#include <sstream>
#include <iomanip>

namespace {
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

void DiscoveryService::initializeSupplierMap() {
    supplier_map_ = {
        {"AWS", {"AWS", "AMZN", "Amazon Web Services"}},
        {"AMAZON WEB SERVICES", {"AWS", "AMZN", "Amazon Web Services"}},
        {"GCP", {"GCP", "GOOGL", "Google Cloud Platform"}},
        {"GOOGLE CLOUD", {"GCP", "GOOGL", "Google Cloud Platform"}},
        {"AZURE", {"AZURE", "MSFT", "Microsoft Azure"}},
        {"MICROSOFT", {"AZURE", "MSFT", "Microsoft Azure"}},
        {"APPLE", {"APPLE", "AAPL", "Apple Inc."}},
        {"NETFLIX", {"NETFLIX", "NFLX", "Netflix"}},
        {"STARBUCKS", {"STARBUCKS", "SBUX", "Starbucks"}},
        {"MCDONALD", {"MCDONALD", "MCD", "McDonald's"}},
        {"AMZN", {"AMZN", "AMZN", "Amazon"}},
        {"AMAZON", {"AMZN", "AMZN", "Amazon"}},
        {"WHOLE FOODS", {"AMZN", "AMZN", "Whole Foods (Amazon)"}},
        {"TARGET", {"TARGET", "TGT", "Target"}},
        {"WALMART", {"WALMART", "WMT", "Walmart"}},
        {"COSTCO", {"COSTCO", "COST", "Costco"}},
        {"HOME DEPOT", {"HOME DEPOT", "HD", "Home Depot"}},
        {"LOWES", {"LOWES", "LOW", "Lowe's"}},
        {"BEST BUY", {"BEST BUY", "BBY", "Best Buy"}},
        {"SPOTIFY", {"SPOTIFY", "SPOT", "Spotify"}},
        {"HULU", {"HULU", "DIS", "Hulu (Disney)"}},
        {"DISNEY", {"DISNEY", "DIS", "Disney"}},
        {"UBER", {"UBER", "UBER", "Uber"}},
        {"LYFT", {"LYFT", "LYFT", "Lyft"}},
        {"DOORDASH", {"DOORDASH", "DASH", "DoorDash"}},
        {"GRUBHUB", {"GRUBHUB", "GRUB", "Grubhub"}},
        {"EXPEDIA", {"EXPEDIA", "EXPE", "Expedia"}},
        {"AIRBNB", {"AIRBNB", "ABNB", "Airbnb"}},
        {"MARRIOTT", {"MARRIOTT", "MAR", "Marriott"}},
        {"HILTON", {"HILTON", "HLT", "Hilton"}},
        {"AT&T", {"ATT", "T", "AT&T"}},
        {"VERIZON", {"VERIZON", "VZ", "Verizon"}},
        {"T-MOBILE", {"T-MOBILE", "TMUS", "T-Mobile"}},
        {"COMCAST", {"COMCAST", "CMCSA", "Comcast"}},
        {"CHIPOTLE", {"CHIPOTLE", "CMG", "Chipotle"}},
        {"PANERA", {"PANERA", "PNRA", "Panera"}},
        {"MORTGAGE", {"MORTGAGE", "WELL", "Wells Fargo Mortgage"}},
        {"CHASE", {"CHASE", "JPM", "JPMorgan Chase"}},
        {"BANK OF AMERICA", {"BOA", "BAC", "Bank of America"}},
        {"CITIBANK", {"CITI", "C", "Citibank"}},
        {"GOLDMAN", {"GS", "GS", "Goldman Sachs"}},
        {"VENMO", {"VENMO", "PYPL", "Venmo (PayPal)"}},
        {"PAYPAL", {"PAYPAL", "PYPL", "PayPal"}},
        {"ZOOM", {"ZOOM", "ZM", "Zoom"}},
        {"ADOBE", {"ADOBE", "ADBE", "Adobe"}},
        {"SALESFORCE", {"SALESFORCE", "CRM", "Salesforce"}},
        {"ORACLE", {"ORACLE", "ORCL", "Oracle"}},
        {"IBM", {"IBM", "IBM", "IBM"}},
        {"INTEL", {"INTEL", "INTC", "Intel"}},
        {"AMD", {"AMD", "AMD", "AMD"}},
        {"NVIDIA", {"NVIDIA", "NVDA", "NVIDIA"}},
        {"TESLA", {"TESLA", "TSLA", "Tesla"}},
        {"HOMEDEPOT.COM", {"HOME DEPOT", "HD", "Home Depot"}},
        {"AMAZON.COM", {"AMZN", "AMZN", "Amazon"}}
    };

    Logger::instance().info("DiscoveryService: Supplier map initialized with " + 
        std::to_string(supplier_map_.size()) + " entries");
}

std::optional<std::string> DiscoveryService::mapToSupplier(const std::string& description) const {
    if (description.empty()) {
        return std::nullopt;
    }

    std::string upper_desc;
    upper_desc.reserve(description.size());
    for (char c : description) {
        upper_desc += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    for (const auto& [keyword, mapping] : supplier_map_) {
        if (upper_desc.find(keyword) != std::string::npos) {
            Logger::instance().debug("DiscoveryService: Matched '" + keyword + "' for description: " + description);
            return mapping.ticker_symbol;
        }
    }

    return std::nullopt;
}

std::vector<VelocityResult> DiscoveryService::calculateVelocity(
    const std::vector<Transaction>& transactions,
    const std::vector<Category>& categories
) const {
    std::map<std::string, double> current_month;
    std::map<std::string, double> previous_month;

    std::string current_month_prefix = month_from_offset(0);
    std::string previous_month_prefix = month_from_offset(-1);

    for (const auto& tx : transactions) {
        if (tx.amount >= 0) continue;

        if (tx.date.substr(0, 7) == current_month_prefix) {
            current_month[tx.category_id] += std::abs(tx.amount);
        } else if (tx.date.substr(0, 7) == previous_month_prefix) {
            previous_month[tx.category_id] += std::abs(tx.amount);
        }
    }

    std::vector<VelocityResult> results;

    for (const auto& [cat_id, current_spend] : current_month) {
        VelocityResult vr;
        vr.category_id = cat_id;
        vr.current_month_spend = current_spend;
        vr.previous_month_spend = previous_month.count(cat_id) ? previous_month[cat_id] : 0.0;

        if (vr.previous_month_spend > 0) {
            vr.percent_change = ((vr.current_month_spend - vr.previous_month_spend) / vr.previous_month_spend) * 100.0;
        } else if (vr.current_month_spend > 0) {
            vr.percent_change = 100.0;
        } else {
            vr.percent_change = 0.0;
        }

        results.push_back(vr);
    }

    Logger::instance().debug("DiscoveryService: Calculated velocity for " + 
        std::to_string(results.size()) + " categories");

    return results;
}