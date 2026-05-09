#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <iostream>
#include <ctime>
#include <memory>
#include <algorithm>

#include "models/DataStore.h"
#include "models/Entity.h"
#include "services/StorageService.h"
#include "services/PlaidService.h"
#include "services/ServiceContainer.h"
#include "utils/Logger.h"
#include "utils/ConfigManager.h"
#include "views/DashboardView.h"
#include "views/AccountsView.h"
#include "views/TransactionsView.h"
#include "views/BudgetView.h"

using namespace ftxui;

std::string get_current_month() {
    time_t now = time(nullptr);
    tm* lt = localtime(&now);
    char buf[8];
    strftime(buf, sizeof(buf), "%Y-%m", lt);
    return std::string(buf);
}

class App {
public:
    ServiceContainer services;
    DataStore data_store;
    std::unique_ptr<DashboardView> dashboard_view;
    std::unique_ptr<AccountsView> accounts_view;
    std::unique_ptr<TransactionsView> transactions_view;
    std::unique_ptr<BudgetView> budget_view;

    int current_entity = 0;
    int current_tab = 0;
    std::string status_message = "";
    std::vector<std::string> tabs = {"Dashboard", "Accounts", "Transactions", "Budget"};

    App() {
        // Load config for API keys
        if (!ConfigManager::instance().load_env_file()) {
            Logger::instance().info("No config found. Use [P] to configure Plaid credentials.");
        }

        auto storage_path = ConfigManager::instance().get_storage_path();
        auto storage = std::make_shared<JsonStorageService>(storage_path);
        services.set_storage(storage);

        auto client_id_opt = ConfigManager::instance().get_plaid_client_id();
        auto secret_opt = ConfigManager::instance().get_plaid_secret();
        auto env_opt = ConfigManager::instance().get_plaid_environment();
        std::string client_id = client_id_opt.has_value() ? *client_id_opt : "";
        std::string secret = secret_opt.has_value() ? *secret_opt : "";

        PlaidEnvironment env = PlaidEnvironment::Sandbox;
        if (env_opt.has_value()) {
            std::string env_str = *env_opt;
            std::transform(env_str.begin(), env_str.end(), env_str.begin(), ::tolower);
            if (env_str == "development") env = PlaidEnvironment::Development;
            else if (env_str == "production") env = PlaidEnvironment::Production;
        }

        bool use_stub = true;
        if (!client_id.empty() && !secret.empty()) {
            use_stub = false;
        }
        auto plaid = create_plaid_service(use_stub);
        if (use_stub) {
            Logger::instance().warning("Using stub Plaid service - no credentials configured");
        }
        plaid->initialize(client_id, secret, env);
        services.set_plaid(plaid);

        data_store.set_storage(storage);

        dashboard_view = std::make_unique<DashboardView>(data_store);
        accounts_view = std::make_unique<AccountsView>(data_store);
        transactions_view = std::make_unique<TransactionsView>(data_store);
        budget_view = std::make_unique<BudgetView>(data_store);

        Logger::instance().info("Application starting...");
        if (!data_store.load()) {
            Logger::instance().warning("No existing data file found, creating new");
        }

        if (data_store.entities.empty()) {
            Entity personal;
            personal.name = "Personal";
            personal.type = EntityType::Individual;
            data_store.add_entity(personal);

            Entity business;
            business.name = "Business LLC";
            business.type = EntityType::LLC;
            data_store.add_entity(business);
        }

        if (data_store.accounts.empty() && !data_store.entities.empty()) {
            Account acc;
            acc.name = "Cash";
            acc.entity_id = data_store.entities[0].id;
            acc.type = AccountType::Cash;
            acc.balance = 100.00;
            acc.institution = "My Wallet";
            data_store.add_account(acc);
        }

        update_views_for_entity();
        data_store.save();
    }

    void update_views_for_entity() {
        if (data_store.entities.empty()) return;

        std::string entity_id = data_store.entities[current_entity].id;
        dashboard_view->set_entity_id(entity_id);
        accounts_view->set_entity_id(entity_id);
        transactions_view->set_entity_id(entity_id);
        budget_view->set_entity_id(entity_id);

        auto accounts = data_store.get_accounts_for_entity(entity_id);
        if (!accounts.empty()) {
            transactions_view->set_account_id(accounts[0]->id);
        }
    }

    void save() {
        if (data_store.save()) {
            Logger::instance().info("Data saved successfully");
        } else {
            Logger::instance().error("Failed to save data");
        }
    }

    std::string get_current_entity_name() {
        if (data_store.entities.empty()) return "None";
        return data_store.entities[current_entity].name;
    }

    Element render() {
        auto tab_content = [this]() -> Element {
            std::string month = get_current_month();
            switch (current_tab) {
                case 0: return dashboard_view->render(month);
                case 1: return accounts_view->render();
                case 2: return transactions_view->render();
                case 3: return budget_view->render(month);
                default: return text("Unknown") | color(LED_BLUE);
            }
        };

        // Build entity tabs
        Elements entity_tabs;
        for (size_t i = 0; i < data_store.entities.size(); ++i) {
            std::string label = " " + std::to_string(i + 1) + ":" + data_store.entities[i].name + " ";
            if (i == (size_t)current_entity) {
                entity_tabs.push_back(text("[x]") | color(LED_BLUE));
                entity_tabs.push_back(text(label) | color(LED_BLUE) | bold);
            } else {
                entity_tabs.push_back(text("[ ]") | color(LED_BLUE_DIM));
                entity_tabs.push_back(text(label) | color(LED_BLUE_DIM));
            }
        }

        // Build view tabs
        Elements view_tabs;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (i == (size_t)current_tab) {
                view_tabs.push_back(text("[x]") | color(LED_BLUE));
                view_tabs.push_back(text(" " + tabs[i] + " ") | color(LED_BLUE) | bold);
            } else {
                view_tabs.push_back(text("[ ]") | color(LED_BLUE_DIM));
                view_tabs.push_back(text(" " + tabs[i] + " ") | color(LED_BLUE_DIM));
            }
        }

        return vbox({
            text("") | bgcolor(Color::Black),
            hbox(entity_tabs) | color(LED_BLUE) | bgcolor(Color::Black),
            hbox(view_tabs) | color(LED_BLUE) | bgcolor(Color::Black),
            separator() | color(LED_BLUE_DIM),
            tab_content() | flex | bgcolor(Color::Black),
            separator() | color(LED_BLUE_DIM),
            text("  [1-2] Switch entity  [Tab] Switch view  [P] Link Plaid  [L] Link test  [C] Config  [Q] Quit") | dim,
            status_message.empty() ? text("") : text("  " + status_message) | color(Color::Green),
        }) | bgcolor(Color::Black) | color(LED_BLUE);
    }
};

int main() {
    auto screen = ScreenInteractive::Fullscreen();

    App app;

    auto component = Renderer([&] {
        return app.render();
    });

    component = CatchEvent(component, [&](Event event) {
        if (event == Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }

        // Entity switching with 1, 2 keys
        if (event == Event::Character('1') && app.data_store.entities.size() >= 1) {
            app.current_entity = 0;
            app.update_views_for_entity();
            return true;
        }
        if (event == Event::Character('2') && app.data_store.entities.size() >= 2) {
            app.current_entity = 1;
            app.update_views_for_entity();
            return true;
        }

        // View tab switching
        if (event == Event::Tab) {
            app.current_tab = (app.current_tab + 1) % app.tabs.size();
            return true;
        }
        if (event == Event::TabReverse) {
            app.current_tab = (app.current_tab - 1 + (int)app.tabs.size()) % app.tabs.size();
            return true;
        }

        // Arrow navigation within views
        if (event == Event::ArrowUp) {
            if (app.current_tab == 1) app.accounts_view->set_selected(app.accounts_view->get_selected() > 0 ? app.accounts_view->get_selected() - 1 : 0);
            if (app.current_tab == 2) app.transactions_view->set_selected(app.transactions_view->get_selected() > 0 ? app.transactions_view->get_selected() - 1 : 0);
            if (app.current_tab == 3) app.budget_view->set_selected(app.budget_view->get_selected() > 0 ? app.budget_view->get_selected() - 1 : 0);
            return true;
        }
        if (event == Event::ArrowDown) {
            if (app.current_tab == 1) app.accounts_view->set_selected(app.accounts_view->get_selected() + 1);
            if (app.current_tab == 2) app.transactions_view->set_selected(app.transactions_view->get_selected() + 1);
            if (app.current_tab == 3) app.budget_view->set_selected(app.budget_view->get_selected() + 1);
            return true;
        }

        // Quit and save
        if (event == Event::Character('q') || event == Event::Character('Q')) {
            app.save();
            screen.ExitLoopClosure()();
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            app.save();
            return true;
        }
        // Link Plaid account
        if (event == Event::Character('p') || event == Event::Character('P')) {
            auto plaid = app.services.get_plaid();
            if (plaid) {
                std::string link_token = plaid->create_link_token();
                app.status_message = "Plaid: Token=" + link_token + " | Use Plaid Sandbox to get public token, then press 'L' to link";
            }
            return true;
        }
        // Link with public token
        if (event == Event::Character('l') || event == Event::Character('L')) {
            auto plaid = app.services.get_plaid();
            if (plaid) {
                auto result = plaid->exchange_public_token("public-sandbox-test");
                if (result) {
                    Account acc;
                    acc.name = "Plaid Account (Sandbox)";
                    acc.entity_id = app.data_store.entities[app.current_entity].id;
                    acc.type = AccountType::Checking;
                    acc.balance = 1000.00;
                    acc.institution = "Plaid Sandbox Bank";
                    app.data_store.add_account(acc);
                    app.update_views_for_entity();
                    app.status_message = "Account linked from Plaid Sandbox!";
                }
            }
            return true;
        }

        // Configure Plaid credentials
        if (event == Event::Character('c') || event == Event::Character('C')) {
            auto client_id = ConfigManager::instance().get_plaid_client_id();
            auto secret = ConfigManager::instance().get_plaid_secret();
            std::string client_status = client_id.has_value() ? "SET" : "NOT SET";
            std::string secret_status = secret.has_value() ? "SET" : "NOT SET";
            app.status_message = "Config: ClientID=[" + client_status + "] Secret=[" + secret_status + "] - Edit .env to configure";
            return true;
        }

        return false;
    });

    screen.Loop(component);
    return 0;
}