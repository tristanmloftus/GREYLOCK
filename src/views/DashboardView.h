#pragma once

// DashboardView — the default TUI view.
//
// Phase 5 rewrite: this view composes five live panels using the FTXUI
// widgets promoted from proposals/ in commit f573278:
//   - ui_net_worth          (per-account-type balances)
//   - ui_category_trends    (top spending categories, 3-month aggregate)
//   - ui_shovel_intelligence (discovered AI-infrastructure suppliers)
//   - ui_shovel_score       (composite 0-100 AI-spend score)
//   - ui_sync_status        (last-sync time + counts)
//
// The view pulls all data from DataStore (the TUI-side cache populated by
// BackendClient sync) and DiscoveryService.  It does NOT hold any
// BackendClient state directly; sync status falls back to DataStore-side
// counts when BackendClient does not yet expose a sync_status() method.

#include <ftxui/dom/elements.hpp>
#include <string>

class DataStore;

class DashboardView {
public:
    explicit DashboardView(DataStore& data_store);

    void set_entity_id(const std::string& id) { entity_id_ = id; }

    // Render the composed dashboard.  current_month is "YYYY-MM".
    ftxui::Element render(const std::string& current_month);

private:
    DataStore& data_store_;
    std::string entity_id_;
};
