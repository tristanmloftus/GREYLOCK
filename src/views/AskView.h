#pragma once

// ---------------------------------------------------------------------------
// AskView — `:ask <query>` output panel (reference Panel 3).
// ---------------------------------------------------------------------------
// Greylock's `:ask` reconciles live state against logged intent. The full
// open-ended NL surface is blocked on Q8 (embedding model) and Q10
// (privacy tiering); this view handles the structured, deterministic
// subset that doesn't need an LLM:
//
//   ask cash position [<entity>]   total balance + operating/treasury/
//                                  deployed splits, sourced from the
//                                  accounts table; framework-drift
//                                  comparison appears when a capital
//                                  framework decision is logged.
//   ask net worth                  signed total over all linked accounts.
//
// Unknown queries surface a small "no handler for this query yet" panel
// that names what's known so the user can self-correct without an error.

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "../models/DataStore.h"
#include "ViewCommon.h"

class AskView {
public:
    explicit AskView(DataStore& data_store) : data_store_(data_store) {}

    // The exact string the user typed after `ask ` (e.g. "pcc cash
    // position", "net worth").  Comparison is case-insensitive and
    // tolerant of extra whitespace.
    void set_query(std::string q)   { query_ = std::move(q); has_query_ = true; }
    void clear()                    { query_.clear(); has_query_ = false; }
    bool has_query() const          { return has_query_; }

    Element render() const {
        using namespace ftxui;
        if (!has_query_) {
            return vbox({
                text(""),
                text("  ask · (no query)") | color(kTokens.fg_emphasized),
                text(""),
                text("  Use `:ask <question>` to query.") | color(kTokens.fg_dim),
            }) | flex;
        }

        std::string normalized = lower(trim(query_));

        // ---- cash position -----------------------------------------
        if (normalized.find("cash position") != std::string::npos
         || normalized.find("cash") != std::string::npos) {
            return render_cash_position(extract_entity_scope(normalized));
        }
        // ---- net worth ---------------------------------------------
        if (normalized.find("net worth") != std::string::npos
         || normalized.find("networth") != std::string::npos) {
            return render_net_worth(extract_entity_scope(normalized));
        }

        return render_unknown();
    }

private:
    DataStore&  data_store_;
    std::string query_;
    bool        has_query_ = false;

    static std::string trim(std::string s) {
        auto lp = s.find_first_not_of(" \t");
        if (lp == std::string::npos) return "";
        auto rp = s.find_last_not_of(" \t");
        return s.substr(lp, rp - lp + 1);
    }
    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Pick out an entity scope hint from the query.  Returns the scope
    // string (e.g. "pcc", "me-rory") or empty if none.  For now, only
    // "pcc" is recognised.
    static std::string extract_entity_scope(const std::string& q) {
        if (q.find("pcc") != std::string::npos) return "pcc";
        if (q.find("me") != std::string::npos)  return "me";
        return "";
    }

    static std::string fmt_dollars(double v) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os.setf(std::ios::fixed); os.precision(0);
        if (v < 0) os << "-$" << std::abs(v);
        else       os << "$" << v;
        return os.str();
    }
    static std::string fmt_dollars_cents(double v) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os.setf(std::ios::fixed); os.precision(2);
        if (v < 0) os << "-$" << std::abs(v);
        else       os << "$" << v;
        return os.str();
    }

    // Render a horizontal bar of N blocks, scaled to `pct` (0-100).
    // Width is fixed at 50 chars so bars are comparable across rows.
    static Element bar(double pct) {
        const int W = 50;
        int filled = static_cast<int>(std::round(pct / 100.0 * W));
        if (filled < 0) filled = 0;
        if (filled > W) filled = W;
        std::string s(filled, char(0));
        std::string t = std::string(filled, ' ');
        // Use ▓ glyphs (#) for the filled portion via repeated symbol.
        std::string glyph;
        for (int i = 0; i < filled; ++i) glyph += "\xE2\x96\x88";  // U+2588 full block
        return text(glyph) | color(kTokens.accent_positive);
    }

    Element render_cash_position(const std::string& scope) const {
        using namespace ftxui;

        // Filter accounts by entity scope.  For "pcc" we match entity
        // name; for "me" we use the user's #me-* entity; for empty, all.
        std::vector<const Account*> in_scope;
        for (const auto& acc : data_store_.accounts) {
            if (scope.empty()) {
                in_scope.push_back(&acc);
                continue;
            }
            // Find the entity by id.
            for (const auto& e : data_store_.entities) {
                if (e.id != acc.entity_id) continue;
                const std::string ename = lower(e.name);
                if (scope == "pcc"
                    && (ename.find("pcc") != std::string::npos
                        || ename.find("platinum creek") != std::string::npos)) {
                    in_scope.push_back(&acc);
                } else if (scope == "me"
                           && (ename.find("personal") != std::string::npos
                               || ename.find("rory") != std::string::npos)) {
                    in_scope.push_back(&acc);
                }
                break;
            }
        }

        // Classify by AccountType into three buckets.  Mapping:
        //   operating  = Checking
        //   treasury   = Savings + Cash
        //   deployed   = Investment + (CreditCard counted as negative)
        double total = 0;
        double operating = 0, treasury = 0, deployed = 0;
        for (auto* a : in_scope) {
            total += a->balance;
            switch (a->type) {
                case AccountType::Checking:   operating += a->balance; break;
                case AccountType::Savings:    treasury  += a->balance; break;
                case AccountType::Cash:       treasury  += a->balance; break;
                case AccountType::Investment: deployed  += a->balance; break;
                case AccountType::CreditCard: deployed  += a->balance; break;
                default: break;
            }
        }

        auto pct = [&](double v) { return total != 0 ? (v / total) * 100.0 : 0; };

        Element title = hbox({
            text(scope.empty() ? "all entities"
               : scope == "pcc" ? "pcc · platinum creek capital"
                                : "#me-" + scope) | color(kTokens.fg_emphasized) | bold,
        });

        Element total_row = hbox({
            text("  total") | color(kTokens.fg_dim) | size(WIDTH, EQUAL, 14),
            filler(),
            text(fmt_dollars_cents(total)) | color(kTokens.fg_emphasized) | bold,
            text("  "),
        });

        auto row = [&](const std::string& label, double v) {
            return hbox({
                text("    " + label) | color(kTokens.fg_default) | size(WIDTH, EQUAL, 14),
                text(fmt_dollars_cents(v)) | size(WIDTH, EQUAL, 14),
                text("  "),
                text(std::to_string(static_cast<int>(std::round(pct(v)))) + "%")
                    | size(WIDTH, EQUAL, 6) | color(kTokens.fg_dim),
                bar(pct(v)),
            });
        };

        // Source attribution line.
        int linked = 0;
        std::string newest_tx_date;
        for (auto* a : in_scope) if (a->is_plaid_linked) ++linked;
        for (const auto& tx : data_store_.transactions) {
            if (tx.date > newest_tx_date) newest_tx_date = tx.date;
        }
        std::string sync_age = "never";
        if (!newest_tx_date.empty()) {
            sync_age = newest_tx_date;
        }

        Element src = hbox({
            text("  source · plaid · "
                 + std::to_string(linked)
                 + " account"  + (linked == 1 ? "" : "s")
                 + " · newest tx " + sync_age)
              | color(kTokens.fg_dim),
        });

        return vbox({
            text(""),
            title,
            separator() | color(kTokens.fg_dim),
            text(""),
            total_row,
            text(""),
            row("operating", operating),
            row("treasury",  treasury),
            row("deployed",  deployed),
            text(""),
            text("  against framework") | color(kTokens.fg_dim),
            text("    (no capital framework decision logged yet — see :open decision <id>)")
              | color(kTokens.fg_dim),
            text(""),
            src,
        }) | flex;
    }

    Element render_net_worth(const std::string& scope) const {
        using namespace ftxui;
        double total = 0;
        for (const auto& acc : data_store_.accounts) {
            if (!scope.empty()) {
                // limit by scope — same logic as cash position
                bool in = false;
                for (const auto& e : data_store_.entities) {
                    if (e.id != acc.entity_id) continue;
                    const std::string ename = lower(e.name);
                    if (scope == "pcc" && (ename.find("pcc") != std::string::npos
                                           || ename.find("platinum creek") != std::string::npos))
                        in = true;
                    if (scope == "me" && (ename.find("personal") != std::string::npos
                                          || ename.find("rory") != std::string::npos))
                        in = true;
                    break;
                }
                if (!in) continue;
            }
            total += acc.balance;
        }

        return vbox({
            text(""),
            text(scope.empty() ? "net worth · all entities"
                : scope == "pcc" ? "net worth · pcc"
                                 : "net worth · #me") | color(kTokens.fg_emphasized) | bold,
            separator() | color(kTokens.fg_dim),
            text(""),
            text("  " + fmt_dollars_cents(total))
              | color(total >= 0 ? kTokens.accent_positive : kTokens.accent_negative)
              | bold,
            text(""),
            text("  source · plaid · live account balances")
              | color(kTokens.fg_dim),
        }) | flex;
    }

    Element render_unknown() const {
        using namespace ftxui;
        return vbox({
            text(""),
            text("  no handler for: " + query_) | color(kTokens.accent_warning),
            text(""),
            text("  known queries (no LLM required):") | color(kTokens.fg_dim),
            text("    :ask cash position [pcc | me]")  | color(kTokens.fg_default),
            text("    :ask net worth [pcc | me]")      | color(kTokens.fg_default),
            text(""),
            text("  open-ended `:ask` arrives once Q8 (embedding model)")
              | color(kTokens.fg_dim),
            text("  and Q10 (privacy tiering) are answered.")
              | color(kTokens.fg_dim),
        }) | flex;
    }
};
