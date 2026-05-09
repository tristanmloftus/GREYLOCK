#pragma once

#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

struct SyncStatus {
    std::string institution;
    bool connected;
    std::string last_sync;
    std::string error_message;
};

namespace ftxui {

Component SyncStatusIndicator(const std::vector<SyncStatus>& statuses);

Element SyncStatusIndicatorRenderer(const std::vector<SyncStatus>& statuses);

} // namespace ftxui