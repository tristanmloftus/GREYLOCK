#include "ui_sync_status.h"

namespace ftxui {

Component SyncStatusIndicator(const std::vector<tf::widgets::SyncStatus>& statuses) {
    return Renderer([statuses] {
        return SyncStatusIndicatorRenderer(statuses);
    });
}

Element SyncStatusIndicatorRenderer(const std::vector<tf::widgets::SyncStatus>& statuses) {
    std::vector<Element> rows;

    rows.push_back(text("Connection Status") | bold);
    rows.push_back(separator());

    if (statuses.empty()) {
        rows.push_back(text("  No accounts linked.") | dim);
        rows.push_back(text("  Press [P] to link a bank.") | dim);
    } else {
        for (const auto& s : statuses) {
            const std::string status_icon = s.connected ? "[+]" : "[x]";
            const Color status_color = s.connected ? Color::Green : Color::Red;

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
