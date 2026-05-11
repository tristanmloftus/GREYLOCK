// ui_sync_status — promoted from proposals/ in Phase 5.
//
// Renders per-institution sync status (last-sync timestamp, connection
// state, error message).  The SyncStatus POD lives in namespace
// tf::widgets.

#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

namespace tf::widgets {

struct SyncStatus {
    std::string institution;
    bool connected;
    std::string last_sync;
    std::string error_message;
};

} // namespace tf::widgets

namespace ftxui {

Component SyncStatusIndicator(const std::vector<tf::widgets::SyncStatus>& statuses);

Element SyncStatusIndicatorRenderer(const std::vector<tf::widgets::SyncStatus>& statuses);

} // namespace ftxui
