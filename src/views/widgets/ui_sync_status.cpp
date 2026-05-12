// ---------------------------------------------------------------------------
// ui_sync_status.cpp — implementation of the "Connection Status" panel.
// ---------------------------------------------------------------------------
// VISUAL
//   A bordered FTXUI vbox.  Header "Connection Status" (bold) +
//   separator, then one hbox row per institution:
//     <icon>  <institution>   <sync_time_or_error>
//   Empty state (no linked accounts) shows two dim onboarding lines:
//     "  No accounts linked."
//     "  Press [P] to link a bank."
//
// FORMATTING RULES
//   - Status icon: "[+]" when connected, "[x]" when disconnected.  ASCII
//     intentionally (FTXUI grapheme-width on real glyphs is fragile in
//     v0.2).
//   - sync_time string assembly:
//       connected   && last_sync non-empty  -> "Synced <last_sync>"
//       connected   && last_sync empty      -> "Never"
//       !connected  && error_message non-empty -> error_message
//       !connected  && error_message empty  -> "Never"
//   - No date reformatting — last_sync is rendered as-is (the upstream
//     "YYYY-MM-DD" string).
//
// COLOR DISCIPLINE (semantics for THIS widget)
//   - Green "[+]" = institution connected and healthy.
//   - Red   "[x]" = institution disconnected (or error state).
//   - Bold (no color) on the institution name.
//   - Dim on the sync_time / error message tail.
//
//   This widget uses the conventional green=good / red=bad mapping,
//   in contrast to ui_category_trends and ui_shovel_intelligence where
//   the inversion applies for expense direction.  The v0.3 redesign
//   must keep this widget on the standard health-status semantics.
//
// EDGE CASES
//   - Empty statuses vector: onboarding empty state (see VISUAL).
//   - Connected institution with no transactions yet: "Never".  This is
//     a transient state on a freshly linked account before its first
//     sync — preserved as a visible cue rather than hidden.
//   - Disconnected institution with no error_message: also "Never",
//     which means "we have no record" rather than "syncing failed".
//
// CALLERS
//   src/views/DashboardView.cpp::render() — once per frame.
// ---------------------------------------------------------------------------

#include "ui_sync_status.h"

namespace ftxui {

// ---------------------------------------------------------------------------
// SyncStatusIndicator
// ---------------------------------------------------------------------------
// Component wrapper around the renderer.  Captures the statuses vector
// by copy so the resulting Component can outlive the call site.
// ---------------------------------------------------------------------------
Component SyncStatusIndicator(const std::vector<tf::widgets::SyncStatus>& statuses) {
    return Renderer([statuses] {
        return SyncStatusIndicatorRenderer(statuses);
    });
}

// ---------------------------------------------------------------------------
// SyncStatusIndicatorRenderer
// ---------------------------------------------------------------------------
// Builds the FTXUI Element graph described in the file header.  Pure
// function.  Called once per frame by DashboardView::render().
// ---------------------------------------------------------------------------
Element SyncStatusIndicatorRenderer(const std::vector<tf::widgets::SyncStatus>& statuses) {
    std::vector<Element> rows;

    rows.push_back(text("Connection Status") | bold);
    rows.push_back(separator());

    if (statuses.empty()) {
        rows.push_back(text("  No accounts linked.") | dim);
        rows.push_back(text("  Press [P] to link a bank.") | dim);
    } else {
        for (const auto& s : statuses) {
            // Pick the icon + color from the connection state.
            const std::string status_icon = s.connected ? "[+]" : "[x]";
            const Color status_color = s.connected ? Color::Green : Color::Red;

            // Assemble the trailing label per the priority order
            // documented in the file header.
            std::string sync_time = s.last_sync.empty() ? "Never" : s.last_sync;
            if (s.connected && !s.last_sync.empty()) {
                sync_time = "Synced " + s.last_sync;
            } else if (!s.connected && !s.error_message.empty()) {
                sync_time = s.error_message;
            }

            Element row = hbox({
                text(status_icon) | color(status_color),
                text(" "),
                text(s.institution) | bold,
                text("  "),
                text(sync_time) | dim
            });
            rows.push_back(row);
        }
    }

    return vbox(std::move(rows)) | border;
}

} // namespace ftxui
