#pragma once

// ---------------------------------------------------------------------------
// HomeView — the tiled 5-panel landing page (reference design).
// ---------------------------------------------------------------------------
// Until 2026-05-15 this view rendered only Panel 1 (morning digest).  The
// new layout per Rory's reference image puts all five reference panels on
// the home surface as a tiled dashboard:
//
//   ┌─────────────────────────┬─────────────────────────┐
//   │ [1] morning digest      │ [2] graph --depth 2     │
//   │                         │                         │
//   ├─────────────────────────┼─────────────────────────┤
//   │ [3] ask pcc cash pos    │ [4] open decision srvc  │
//   │                         │                         │
//   ├─────────────────────────┴─────────────────────────┤
//   │ [5] open cade (spans the bottom row)              │
//   │                                                   │
//   ╰───────────────────────────────────────────────────╯
//
// HomeView is a composer.  It holds non-owning pointers to the four
// content views (GraphView, AskView, DecisionDetailView,
// RelationshipDetailView) and asks each to render itself in tile mode.
// Panel 1 (morning digest) is rendered inline because its content is
// not shared with any other view today.  The App is responsible for
// pre-populating the four content views' state before each home render
// (decision[services-arm], relationship[cade], graph counts, pcc cash).
//
// Layout discipline (greylock-spec.md §8.1 / style-audit.md):
//   - monospace single-weight, hierarchy via color + whitespace only
//   - middle-dot `·` separator
//   - box-drawing chars for trees + rules
//   - block chars for bars + cursor
//   - color tokens: green = data/ok, amber = warning, red = relational
//     debt headers, dim gray = metadata, light gray = body
//   - prompt strip `rory@greylock:~ $ ▮` per tile that simulates a shell
//   - badge `[N] <title>` in each tile's top-left corner
//
// Responsive sizing: at terminal_width >= 180 cols, the 2×2 + bottom-
// spanning grid is rendered; below that, panels stack vertically (one
// per row) but keep the same internal styling.

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../models/DataStore.h"
#include "AskView.h"
#include "DecisionDetailView.h"
#include "GraphView.h"
#include "RelationshipDetailView.h"
#include "ViewCommon.h"

class HomeView {
public:
    explicit HomeView(DataStore& data_store) : data_store_(data_store) {}

    // App-supplied dependencies (non-owning).  All four views must be
    // live before set_*() — App constructs them in the same ctor pass.
    void set_tile_views(GraphView*               graph,
                        AskView*                 ask,
                        tf::views::DecisionDetailView* decision,
                        tf::views::RelationshipDetailView* relationship) {
        graph_       = graph;
        ask_         = ask;
        decision_    = decision;
        relationship_= relationship;
    }

    void set_entity_id(const std::string& id) { entity_id_ = id; }
    void set_user_email(const std::string& e) { user_email_ = e; }
    void set_user_handle(const std::string& h) { user_handle_ = h; }
    void set_scope_tags(const std::string& s)  { scope_tags_override_ = s;  }

    // Min width below which we fall back to a stacked vertical layout.
    static constexpr int kTiledMinCols = 180;

    Element render() const {
        using namespace ftxui;

        // ---- shared header strip --------------------------------------
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char clock_buf[16];
        std::strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &lt);

        std::string scope_tags = scope_tags_override_;
        if (scope_tags.empty()) {
            bool has_pcc = false;
            for (const auto& e : data_store_.entities) {
                const std::string& n = e.name;
                if (n == "PCC"
                 || n == "Platinum Creek Capital"
                 || n == "Platinum Creek Capital LLC") {
                    has_pcc = true;
                    break;
                }
            }
            scope_tags = has_pcc
                ? std::string("#pcc + #me-") + user_handle_
                : std::string("#me-") + user_handle_;
        }
        const std::string header_left =
            std::string("greylock · ") + user_handle_ + "@greylock"
            + (scope_tags.empty() ? std::string() : (" · " + scope_tags));

        Element header = hbox({
            text(header_left) | color(kTokens.fg_emphasized),
            filler(),
            text(std::string(clock_buf)) | color(kTokens.fg_dim),
        });

        // ---- four tiles ----------------------------------------------
        Element tile1 = wrap_tile(1, "morning digest",
                                   render_digest_inline(lt));
        Element tile2 = wrap_tile(2, "graph --depth 2",
                                   graph_ ? graph_->render_tile()
                                          : tile_placeholder("graph not loaded"));
        Element tile3 = wrap_tile(3, "ask pcc cash position",
                                   ask_ ? ask_->render_tile()
                                        : tile_placeholder("ask not loaded"));
        Element tile4 = wrap_tile(4, "open decision services-arm",
                                   decision_ ? decision_->render_tile()
                                             : tile_placeholder("decision not loaded"));
        Element tile5 = wrap_tile(5, "open cade",
                                   relationship_ ? relationship_->render_tile()
                                                 : tile_placeholder("relationship not loaded"));

        // ---- status footer (live data summary) -----------------------
        const int n_entities = static_cast<int>(data_store_.entities.size());
        const int n_accounts = static_cast<int>(data_store_.accounts.size());
        std::ostringstream footer_os;
        footer_os << "  " << alert_count() << " alert"
                  << (alert_count() == 1 ? "" : "s")
                  << " · synced " << sync_age()
                  << " · " << n_entities << " entit" << (n_entities == 1 ? "y" : "ies")
                  << " · " << n_accounts << " account" << (n_accounts == 1 ? "" : "s");
        Element status_footer = text(footer_os.str()) | color(kTokens.fg_dim);

        // ---- responsive layout ---------------------------------------
        // ftxui::Terminal::Size() returns the current terminal dimensions
        // measured on every Screen::Loop iteration, so this branch is
        // re-evaluated each frame.
        const int term_cols = ftxui::Terminal::Size().dimx;
        Element body;
        if (term_cols >= kTiledMinCols) {
            // 2×2 + bottom-spanning row.
            Element top_row = hbox({
                tile1 | flex,
                separator() | color(kTokens.fg_dim),
                tile2 | flex,
            }) | flex;
            Element mid_row = hbox({
                tile3 | flex,
                separator() | color(kTokens.fg_dim),
                tile4 | flex,
            }) | flex;
            body = vbox({
                top_row,
                separator() | color(kTokens.fg_dim),
                mid_row,
                separator() | color(kTokens.fg_dim),
                tile5 | flex,
            }) | flex;
        } else {
            // Stacked: one tile per row.
            body = vbox({
                tile1,
                separator() | color(kTokens.fg_dim),
                tile2,
                separator() | color(kTokens.fg_dim),
                tile3,
                separator() | color(kTokens.fg_dim),
                tile4,
                separator() | color(kTokens.fg_dim),
                tile5,
            }) | flex;
        }

        return vbox({
            header,
            separator() | color(kTokens.fg_dim),
            body,
            separator() | color(kTokens.fg_dim),
            status_footer,
        }) | flex;
    }

private:
    DataStore&  data_store_;
    std::string entity_id_;
    std::string user_email_;
    std::string user_handle_{"rory"};
    std::string scope_tags_override_;

    // Non-owning pointers to the four content views.  May be null until
    // App calls set_tile_views().  When null, the tile renders a dim
    // placeholder so the layout shape stays stable.
    GraphView*                          graph_        = nullptr;
    AskView*                            ask_          = nullptr;
    tf::views::DecisionDetailView*      decision_     = nullptr;
    tf::views::RelationshipDetailView*  relationship_ = nullptr;

    // ---- tile chrome ---------------------------------------------------
    // Each tile carries a numbered badge top-left and a shell prompt
    // strip at the bottom.  Body comes from the underlying view's
    // render_tile() (or render_digest_inline for panel 1).
    Element wrap_tile(int n, const std::string& cmd, Element body) const {
        using namespace ftxui;
        const std::string badge_text = "[" + std::to_string(n) + "]";
        Element badge_row = hbox({
            text("  " + badge_text + " ") | color(kTokens.accent_info) | bold,
            text(cmd) | color(kTokens.fg_dim),
        });
        Element prompt = hbox({
            text("  ") | color(kTokens.fg_dim),
            text(user_handle_ + "@greylock:~ $ ")  | color(kTokens.fg_emphasized),
            text(cmd) | color(kTokens.fg_default),
            text(" ") | color(kTokens.fg_dim),
            text("▮") | blink | color(kTokens.fg_emphasized),
        });
        return vbox({
            badge_row,
            body | flex,
            prompt,
        });
    }

    Element tile_placeholder(const std::string& msg) const {
        using namespace ftxui;
        return vbox({
            text(""),
            text("  " + msg) | color(kTokens.fg_dim),
            filler(),
        }) | flex;
    }

    // ---- panel 1 (morning digest) inline body --------------------------
    Element render_digest_inline(const std::tm& lt) const {
        using namespace ftxui;
        const int hour = lt.tm_hour;
        const std::string greeting =
            (hour < 5)  ? "still up, rory?" :
            (hour < 12) ? "good morning, rory." :
            (hour < 17) ? "good afternoon, rory." :
            (hour < 22) ? "good evening, rory." :
                          "late night, rory.";

        // Digest bullets — same logic as the legacy HomeView, kept inline
        // because no other view consumes "since you were last here".
        std::vector<std::string> bullets;
        {
            const auto& accounts = data_store_.accounts;
            const auto& txs      = data_store_.transactions;
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

        Elements rows;
        rows.push_back(text(""));
        rows.push_back(text("  " + greeting) | color(kTokens.fg_default));
        rows.push_back(text(""));
        if (bullets.empty()) {
            rows.push_back(
                text("  no activity yet — link a bank with [P] on Accounts.")
                | color(kTokens.fg_dim));
        } else {
            rows.push_back(text("  since you were last here:") | color(kTokens.fg_dim));
            for (const auto& b : bullets) {
                rows.push_back(text("    · " + b) | color(kTokens.fg_default));
            }
        }
        rows.push_back(filler());
        return vbox(std::move(rows)) | flex;
    }

    // ---- helpers -------------------------------------------------------
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

    int alert_count() const {
        int n = 0;
        for (const auto& a : data_store_.accounts) {
            if (a.is_plaid_linked && a.balance == 0.0) ++n;
        }
        return n;
    }

    std::string sync_age() const {
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
