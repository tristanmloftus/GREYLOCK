#pragma once

// ---------------------------------------------------------------------------
// HomeView — morning-digest landing page (reference Panel 1).
// ---------------------------------------------------------------------------
// Layout (top to bottom):
//   greylock · rory@greylock · <scope tags>                  HH:MM:SS
//   ─────────────────────────────────────────────────────────────────
//
//   <time-aware greeting>, rory.
//
//   since you were last here:
//     · <bullet>
//     · <bullet>
//     · <bullet>
//
//   ─────────────────────────────────────────────────────────────────
//   rory@greylock:~ $
//   ─────────────────────────────────────────────────────────────────
//   <N alerts> · synced <Nm> ago · <N entities> · <N accounts>
//
// The view is a pure function over DataStore + ConfigManager + the
// current entity/user context. Aggregations happen on every render
// (cheap). The REPL prompt is chrome only; actual command input flows
// through the `:` palette per spec §8.1.

#include <ftxui/dom/elements.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../models/DataStore.h"
#include "ViewCommon.h"

class HomeView {
public:
    explicit HomeView(DataStore& data_store) : data_store_(data_store) {}

    void set_entity_id(const std::string& id) { entity_id_ = id; }
    void set_user_email(const std::string& e) { user_email_ = e; }
    void set_user_handle(const std::string& h) { user_handle_ = h; }
    void set_scope_tags(const std::string& s)  { scope_tags_ = s;  }

    Element render() const {
        using namespace ftxui;

        // ---- 1. Header chrome --------------------------------------
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char clock_buf[16];
        std::strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &lt);

        // Reference: "greylock · rory@greylock · #pcc + #me-rory  09:42:17"
        const std::string header_left =
            std::string("greylock · ") + user_handle_ + "@greylock"
            + (scope_tags_.empty() ? std::string() : (" · " + scope_tags_));

        Element header = hbox({
            text(header_left) | color(kTokens.fg_emphasized),
            filler(),
            text(std::string(clock_buf)) | color(kTokens.fg_dim),
        });

        // ---- 2. Greeting + digest ----------------------------------
        const int hour = lt.tm_hour;
        const std::string greeting =
            (hour < 5)  ? "still up, rory?" :
            (hour < 12) ? "good morning, rory." :
            (hour < 17) ? "good afternoon, rory." :
            (hour < 22) ? "good evening, rory." :
                          "late night, rory.";

        // Compute digest bullets from the in-memory DataStore.  These
        // are the only signals the TUI has today without a backend
        // audit-log feed; richer "since you were last here" content
        // lands when we wire GET /audit/recent.
        std::vector<std::string> bullets;
        {
            const auto& accounts = data_store_.accounts;
            const auto& txs      = data_store_.transactions;

            // Bullet: # accounts + total balance.
            double total_bal = 0;
            int    linked    = 0;
            for (const auto& a : accounts) {
                total_bal += a.balance;
                if (a.is_plaid_linked) ++linked;
            }
            if (!accounts.empty()) {
                std::ostringstream os;
                os.setf(std::ios::fixed); os.precision(2);
                os << linked << " linked account"
                   << (linked == 1 ? "" : "s")
                   << " · balance $" << total_bal;
                bullets.push_back(os.str());
            }

            // Bullet: last 7 days tx count.
            int recent_count = 0;
            std::string newest_date;
            for (const auto& tx : txs) {
                if (tx.date > newest_date) newest_date = tx.date;
                if (days_ago(tx.date) <= 7) ++recent_count;
            }
            if (recent_count > 0) {
                std::ostringstream os;
                os << recent_count << " transaction"
                   << (recent_count == 1 ? "" : "s")
                   << " in the last 7 days";
                if (!newest_date.empty()) os << " · newest " << newest_date;
                bullets.push_back(os.str());
            }

            // Bullet: current-month cash flow snapshot.
            const std::string month = current_month();
            double income = 0, expense = 0;
            for (const auto& tx : txs) {
                if (tx.date.substr(0, 7) != month) continue;
                if (tx.amount >= 0) income  += tx.amount;
                else                expense += -tx.amount;
            }
            if (income > 0 || expense > 0) {
                std::ostringstream os;
                os.setf(std::ios::fixed); os.precision(2);
                const double net = income - expense;
                os << "cash flow this month: " << (net >= 0 ? "+$" : "-$")
                   << std::abs(net) << " (income $" << income
                   << " · expenses $" << expense << ")";
                bullets.push_back(os.str());
            }
        }

        Elements digest_rows;
        digest_rows.push_back(text(""));
        digest_rows.push_back(text("  " + greeting) | color(kTokens.fg_default));
        digest_rows.push_back(text(""));
        if (bullets.empty()) {
            digest_rows.push_back(
                text("  no activity yet — link a bank with [P] on Accounts.")
                | color(kTokens.fg_dim));
        } else {
            digest_rows.push_back(text("  since you were last here:") | color(kTokens.fg_dim));
            for (const auto& b : bullets) {
                digest_rows.push_back(text("    · " + b) | color(kTokens.fg_default));
            }
        }
        digest_rows.push_back(text(""));

        // ---- 3. REPL prompt strip ----------------------------------
        Element prompt = hbox({
            text("  ") | color(kTokens.fg_dim),
            text(user_handle_ + "@greylock:~ $ ") | color(kTokens.fg_emphasized),
            text("_") | blink | color(kTokens.fg_emphasized),
        });

        // ---- 4. Status footer --------------------------------------
        const int n_entities = static_cast<int>(data_store_.entities.size());
        const int n_accounts = static_cast<int>(data_store_.accounts.size());

        std::ostringstream footer_os;
        footer_os << "  " << alert_count() << " alert"
                  << (alert_count() == 1 ? "" : "s")
                  << " · synced " << sync_age()
                  << " · " << n_entities << " entit" << (n_entities == 1 ? "y" : "ies")
                  << " · " << n_accounts << " account" << (n_accounts == 1 ? "" : "s");

        Element footer = text(footer_os.str()) | color(kTokens.fg_dim);

        // ---- compose -----------------------------------------------
        return vbox({
            header,
            separator() | color(kTokens.fg_dim),
            vbox(std::move(digest_rows)),
            filler(),
            separator() | color(kTokens.fg_dim),
            prompt,
            separator() | color(kTokens.fg_dim),
            footer,
        }) | flex;
    }

private:
    DataStore&  data_store_;
    std::string entity_id_;
    std::string user_email_;
    std::string user_handle_{"rory"};
    std::string scope_tags_;

    // ---- helpers ------------------------------------------------
    static std::string current_month() {
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char b[8];
        std::strftime(b, sizeof(b), "%Y-%m", &lt);
        return std::string(b);
    }

    // crude days-ago — relies on YYYY-MM-DD strings being lex-orderable
    static int days_ago(const std::string& yyyy_mm_dd) {
        if (yyyy_mm_dd.size() < 10) return 9999;
        std::tm tm_tx{};
        tm_tx.tm_year = std::atoi(yyyy_mm_dd.substr(0, 4).c_str()) - 1900;
        tm_tx.tm_mon  = std::atoi(yyyy_mm_dd.substr(5, 2).c_str()) - 1;
        tm_tx.tm_mday = std::atoi(yyyy_mm_dd.substr(8, 2).c_str());
        tm_tx.tm_isdst = -1;
#ifdef _WIN32
        std::time_t t_tx = _mkgmtime(&tm_tx);
#else
        std::time_t t_tx = timegm(&tm_tx);
#endif
        std::time_t now  = std::time(nullptr);
        if (t_tx <= 0) return 9999;
        return static_cast<int>((now - t_tx) / 86400);
    }

    // Placeholder until /audit/recent + sync-status endpoints ship.
    int alert_count() const {
        // For now: count accounts that are linked but have $0 balance
        // (i.e., balance_cents wasn't populated or sync stalled).
        int n = 0;
        for (const auto& a : data_store_.accounts) {
            if (a.is_plaid_linked && a.balance == 0.0) ++n;
        }
        return n;
    }

    std::string sync_age() const {
        // Approximate "synced" as "newest tx date" until /sync/status ships.
        std::string newest;
        for (const auto& tx : data_store_.transactions) {
            if (tx.date > newest) newest = tx.date;
        }
        if (newest.empty()) return "never";
        int d = days_ago(newest);
        if (d <= 0) return "today";
        if (d == 1) return "1d ago";
        return std::to_string(d) + "d ago";
    }
};
