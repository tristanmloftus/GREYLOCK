#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <string>
#include <string_view>

#ifndef _WIN32
#  include <termios.h>
#  include <unistd.h>
#  include <signal.h>
#else
#  include <conio.h>
#endif

#include <sodium.h>

#include "models/DataStore.h"
#include "models/Entity.h"
#include "services/StorageService.h"
#include "services/RemoteBackendStorageService.h"
#include "services/PlaidService.h"
#include "services/ServiceContainer.h"
#include "services/SecurityService.h"
#include "services/http/CurlHttpClient.h"
#include "services/BackendClient.h"
#include "services/AuthService.h"
#include "utils/Logger.h"
#include "utils/ConfigManager.h"
#include "views/DashboardView.h"
#include "views/AccountsView.h"
#include "views/TransactionsView.h"
#include "views/BudgetView.h"
#include "views/FocusController.h"
#include "views/drills/Drill_NetWorth.h"
#include "views/CommandPalette.h"
#include "views/HelpOverlay.h"
#include "views/StatusBar.h"
#include "utils/CommandRegistry.h"
#include "migration/V01Migrator.h"

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

    // Q14 (greylock-spec.md §11): vim-style `g`+letter top-level
    // navigation.  When the user presses `g`, we enter a one-shot
    // pending state; the next keypress maps to a top-level view
    // (g d → Dashboard, g a → Accounts, g t → Transactions,
    //  g b → Budget) and resets the state.  Any unrecognised
    // follow-up just clears the pending state and falls through to
    // the normal handler chain.
    bool g_pending = false;

    // Task v0.3-1: Dashboard focus state machine.  Routes Tab/Shift-Tab/
    // hjkl/arrows/Esc when current_tab == 0 (Dashboard) BEFORE any legacy
    // view-specific handlers.  Reset on view-switch so a return to
    // Dashboard starts at the no-widget-focused state.
    tf::views::FocusController focus_;

    // Task v0.3-4: modal overlay components.  Open via ":" (palette) and
    // "?" (help).  Event routing: when EITHER modal is open it sees the
    // event first and consumes it (palette: typing + selection + dispatch;
    // help: only Esc).  Esc closes whichever modal is open AND calls
    // focus_.exit_modal() so the controller leaves Modal level.  See
    // docs/UI_REDESIGN_V0.3.md §3c, Appendix C.2, Appendix C.3.
    tf::views::CommandPalette palette_;
    tf::views::HelpOverlay    help_;
    tf::views::StatusBar      status_bar_;

    // Closure for screen exit -- wired in main() so dispatch(Quit) can
    // gracefully terminate the FTXUI loop.  Defaults to a no-op until
    // main() installs the real closure.
    std::function<void()> on_exit_;

    App() {
        // Load config for API keys
        if (!ConfigManager::instance().load_env_file()) {
            Logger::instance().info("No config found. Use [P] to configure Plaid credentials.");
        }

        auto storage_path = ConfigManager::instance().get_storage_path();
        auto storage = std::make_shared<JsonStorageService>(storage_path);
        services.set_storage(storage);

        // Wire the HTTP client (libcurl). Used by PlaidService and future services.
        auto http_client = std::make_shared<CurlHttpClient>();
        services.set_http_client(http_client);

        // Wire BackendClient.  Base URL from TF_BACKEND_URL env var (default
        // https://localhost:8443).  Phase 3 will authenticate and pass session
        // tokens; for Phase 2 the client is registered but not actively used.
        {
            const char* backend_url_env = std::getenv("TF_BACKEND_URL");
            std::string backend_url = (backend_url_env && backend_url_env[0] != '\0')
                ? std::string(backend_url_env)
                : "https://localhost:8443";
            try {
                auto backend = std::make_shared<BackendClient>(http_client, backend_url);
                services.set_backend_client(backend);
            } catch (const std::invalid_argument& e) {
                Logger::instance().warning(
                    std::string("BackendClient not registered: ") + e.what());
            }
        }

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

        auto backend = services.get_backend_client();
        auto plaid = create_plaid_service(backend);
        if (!backend) {
            Logger::instance().warning("Using stub Plaid service - no backend configured");
        }
        services.set_plaid(plaid);

        // Wire platform-appropriate secret store.
#ifdef _WIN32
        services.set_secret_store(std::make_shared<DpapiSecretStore>());
#elif defined(__APPLE__)
        services.set_secret_store(std::make_shared<KeychainSecretStore>());
#else
        services.set_secret_store(std::make_shared<FileSecretStore>());
#endif

        // If a cached session token is available for $TF_USER_EMAIL, prefer
        // the backend as the storage source — that's where the canonical
        // user data lives. Falls back to the local JSON file when no
        // session is cached (first-run / pre-login UX).
        std::shared_ptr<IStorageService> effective_storage = storage;
        {
            const char* env_email = std::getenv("TF_USER_EMAIL");
            std::string email = (env_email && env_email[0] != '\0')
                ? std::string(env_email) : "";
            auto secrets = services.get_secret_store();
            auto backend_ptr = services.get_backend_client();
            if (!email.empty() && secrets && backend_ptr) {
                auto raw = secrets->get("tf-session-" + email);
                if (raw.has_value()) {
                    std::string token;
                    token.reserve(raw->size());
                    for (auto b : *raw) token += static_cast<char>(b);
                    effective_storage = std::make_shared<RemoteBackendStorageService>(
                        backend_ptr, std::move(token));
                    services.set_storage(effective_storage);
                    Logger::instance().info(
                        "Storage: using RemoteBackendStorageService (cached session)");
                }
            }
        }

        data_store.set_storage(effective_storage);

        dashboard_view = std::make_unique<DashboardView>(data_store);
        accounts_view = std::make_unique<AccountsView>(data_store);
        transactions_view = std::make_unique<TransactionsView>(data_store);
        budget_view = std::make_unique<BudgetView>(data_store);

        Logger::instance().info("Application starting...");
        if (!data_store.load()) {
            Logger::instance().warning("No existing data file found, creating new");
        }

        // No first-run seeding. Entities and accounts are owned by the
        // backend in a real-storage deployment; in the local-JSON
        // fallback the user starts with an empty workspace and adds
        // entities deliberately via the (still-pending) onboarding
        // flow. See greylock-kickoff.md §3.2.

        update_views_for_entity();
        data_store.save();

        // Wire the command palette's dispatcher.  The palette is data-
        // only; the App provides the behaviour for each CommandId.
        palette_.set_dispatcher([this](tf::utils::CommandId id) {
            dispatch(id);
        });
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

    // ---------------------------------------------------------------------
    // Modal lifecycle helpers (Task v0.3-4).
    //
    // open_palette / open_help do BOTH the modal's open() and the focus
    // controller's enter_modal() so the App's UI invariants stay in
    // lock-step.  close_modals() symmetrically closes whichever modal
    // (or both) is open and tells the focus controller to exit.
    // ---------------------------------------------------------------------
    void open_palette() {
        if (palette_.is_open() || help_.is_open()) return;
        palette_.open();
        focus_.enter_modal();
    }

    void open_help() {
        if (palette_.is_open() || help_.is_open()) return;
        help_.open();
        focus_.enter_modal();
    }

    void close_modals() {
        const bool was_open = palette_.is_open() || help_.is_open();
        palette_.close();
        help_.close();
        if (was_open) focus_.exit_modal();
    }

    // ---------------------------------------------------------------------
    // dispatch — map a CommandId from the palette onto a concrete app
    // action.  The registry is data-only on purpose; ALL behaviour
    // lives here so tests for the registry don't pull in DataStore.
    // ---------------------------------------------------------------------
    void dispatch(tf::utils::CommandId id) {
        using tf::utils::CommandId;
        switch (id) {
            case CommandId::SwitchView_Dashboard:
                current_tab = 0; focus_.reset(); return;
            case CommandId::SwitchView_Accounts:
                current_tab = 1; focus_.reset(); return;
            case CommandId::SwitchView_Transactions:
                current_tab = 2; focus_.reset(); return;
            case CommandId::SwitchView_Budget:
                current_tab = 3; focus_.reset(); return;

            case CommandId::SwitchEntity_Personal:
                if (data_store.entities.size() >= 1) {
                    current_entity = 0; update_views_for_entity();
                }
                return;
            case CommandId::SwitchEntity_Business:
                if (data_store.entities.size() >= 2) {
                    current_entity = 1; update_views_for_entity();
                }
                return;

            case CommandId::LinkPlaid:
                status_message = "Plaid: Use 'L' to link an account "
                                 "via the server endpoint.";
                return;
            case CommandId::LinkPlaidTest: {
                auto plaid = services.get_plaid();
                if (plaid && !data_store.entities.empty() &&
                    !data_store.accounts.empty()) {
                    const auto& acc = data_store.accounts[0];
                    const bool ok = plaid->link_account(
                        acc.id, "public-sandbox-test");
                    status_message = ok ? "Account linked via server."
                                        : "Link failed: " +
                                          plaid->get_last_error();
                } else {
                    status_message = "No accounts to link.";
                }
                return;
            }

            case CommandId::OpenConfig: {
                auto client_id = ConfigManager::instance().get_plaid_client_id();
                auto secret    = ConfigManager::instance().get_plaid_secret();
                std::string c  = client_id.has_value() ? "SET" : "NOT SET";
                std::string s  = secret.has_value()    ? "SET" : "NOT SET";
                status_message = "Config: ClientID=[" + c + "] Secret=["
                                 + s + "] - Edit .env to configure";
                return;
            }

            case CommandId::Quit:
                save();
                if (on_exit_) on_exit_();
                return;

            // Drill-into commands: focus the corresponding widget on the
            // Dashboard.  Actual drill-down panels land in v0.3-2/-3.
            case CommandId::DrillInto_NetWorth:
            case CommandId::DrillInto_SyncStatus:
            case CommandId::DrillInto_CategoryTrends:
                current_tab = 0;
                focus_.reset();
                // Walk Tab presses until the right widget is focused.
                // Order matches WidgetId enum -- see FocusController.h.
                {
                    int steps = 0;
                    switch (id) {
                        case CommandId::DrillInto_NetWorth:       steps = 1; break;
                        case CommandId::DrillInto_SyncStatus:     steps = 2; break;
                        case CommandId::DrillInto_CategoryTrends: steps = 3; break;
                        default: break;
                    }
                    for (int i = 0; i < steps; ++i) {
                        focus_.handle_key(ftxui::Event::Tab);
                    }
                }
                return;

            case CommandId::Help:
                open_help();
                return;

            case CommandId::Logout:
                status_message = "Logout: run TerminalFinance --logout from CLI.";
                return;
            case CommandId::Whoami:
                status_message = "Whoami: run TerminalFinance --whoami from CLI.";
                return;
            case CommandId::Refresh:
                status_message = "Refresh: backend sync not wired in v0.3-4.";
                return;
            case CommandId::Search_Transactions:
                // Search box lands in a later v0.3 task; until then we
                // route the user to the Transactions view and leave a
                // hint in the status bar.
                current_tab = 2; focus_.reset();
                status_message = "Search transactions: type in the "
                                 "Transactions view (search box: v0.3+).";
                return;
        }
    }

    std::string get_current_entity_name() {
        if (data_store.entities.empty()) return "None";
        return data_store.entities[current_entity].name;
    }

    Element render() {
        auto tab_content = [this]() -> Element {
            std::string month = get_current_month();
            // Task v0.3-2: when a Dashboard widget has been drilled into,
            // render the corresponding full-screen drill view instead of
            // the Dashboard grid.  Esc (handled in CatchEvent below) pops
            // the drill via FocusController::exit_drill() and the next
            // frame falls back to dashboard_view->render().
            if (current_tab == 0 &&
                focus_.level() == tf::views::FocusLevel::Drill) {
                const std::string entity_id =
                    data_store.entities.empty()
                        ? std::string()
                        : data_store.entities[current_entity].id;
                switch (focus_.drilled_widget()) {
                    case tf::views::WidgetId::NetWorth: {
                        tf::views::drills::Drill_NetWorth d(
                            data_store, entity_id);
                        return d.render();
                    }
                    // SyncStatus / CategoryTrends drill views are not
                    // yet wired (Drill_ShovelScore was removed in the
                    // 2026-05-16 shovel scrub).  Unrecognised drills
                    // fall back to the Dashboard render gracefully.
                    default:
                        break;
                }
            }
            switch (current_tab) {
                case 0: return dashboard_view->render(month, &focus_);
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

        // Task v0.3-4: status bar is now a class (StatusBar) that owns
        // the two-row layout.  v0.3-1 left context_hints() empty; v0.3-5
        // will populate it.  The StatusBar reads from focus_ on every
        // render so additive changes don't churn this call site.
        const std::string current_view_name =
            (current_tab >= 0 && current_tab < (int)tabs.size())
                ? tabs[(size_t)current_tab]
                : std::string("Unknown");
        Element status_bar_element = status_bar_.render(focus_, current_view_name);

        // ----------------------------------------------------------------
        // Background view -- composes the existing entity/view tabs, the
        // active tab content, and the (extracted) status bar.  IMPORTANT:
        // the bottom row matches the pre-v0.3-4 status string byte-for-
        // byte so existing snapshot fixtures of the App do not regress.
        // The "hints_line" placeholder kept by v0.3-1 is the StatusBar's
        // top row now.
        // ----------------------------------------------------------------
        Element background = vbox({
            text("") | bgcolor(Color::Black),
            hbox(entity_tabs) | color(LED_BLUE) | bgcolor(Color::Black),
            hbox(view_tabs) | color(LED_BLUE) | bgcolor(Color::Black),
            separator() | color(LED_BLUE_DIM),
            tab_content() | flex | bgcolor(Color::Black),
            separator() | color(LED_BLUE_DIM),
            status_bar_element,
            status_message.empty() ? text("") : text("  " + status_message) | color(Color::Green),
        }) | bgcolor(Color::Black) | color(LED_BLUE);

        // ----------------------------------------------------------------
        // Modal overlay (Task v0.3-4) -- if either palette or help is
        // open, dbox composites it on top of the background.  Centered
        // via filler() padding.  See docs/UI_REDESIGN_V0.3.md Appendix
        // C.2.
        // ----------------------------------------------------------------
        if (palette_.is_open()) {
            Element overlay = vbox({
                filler(),
                hbox({ filler(), palette_.render(), filler() }),
                filler(),
            });
            return dbox({ background, overlay });
        }
        if (help_.is_open()) {
            Element overlay = vbox({
                filler(),
                hbox({ filler(), help_.render(), filler() }),
                filler(),
            });
            return dbox({ background, overlay });
        }
        return background;
    }
};

// --------------------------------------------------------------------------
// CLI helpers
// --------------------------------------------------------------------------

// Read a line from stdin with the prompt printed to stdout.
// The input IS echoed to the terminal (passphrase echo-disable is a v0.3 task).
static std::string prompt_line(const std::string& prompt_text) {
    std::cout << prompt_text << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

// Read a passphrase from stdin with echo suppressed.
//
// RC-2: Uses an RAII EchoGuard (POSIX) to guarantee echo is restored on any
// exit path — including exception unwind.  A SIGINT handler is installed via
// sigaction(SA_RESETHAND) so that Ctrl-C also restores the terminal before
// re-raising SIGINT to terminate the process.
//
// Windows note: _getch reads without echo by design; the console subsystem
// intercepts Ctrl-C normally.  No analogous "stuck in raw mode" risk exists on
// the Windows path, so no extra handling is required there.
#ifndef _WIN32

// Signal-time state — must be file-scope so the signal handler can reach it.
static struct termios g_saved_for_signal;
static volatile sig_atomic_t g_signal_active = 0;

static void restore_echo_on_signal(int sig) {
    if (g_signal_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_for_signal);
        g_signal_active = 0;
    }
    // SA_RESETHAND already reset the disposition; re-raise for default action.
    signal(sig, SIG_DFL);
    raise(sig);
}

#endif // !_WIN32

static std::string prompt_passphrase(const std::string& prompt_text) {
#ifndef _WIN32
    // RAII guard: saves termios on construction, disables echo, restores on
    // destruction (handles normal returns AND exception unwind).
    struct EchoGuard {
        struct termios saved;
        bool active = false;
        EchoGuard() {
            if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
                struct termios noecho = saved;
                noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
                if (tcsetattr(STDIN_FILENO, TCSANOW, &noecho) == 0)
                    active = true;
            }
        }
        ~EchoGuard() {
            if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        }
    };

    EchoGuard guard;

    // Install a one-shot SIGINT handler so Ctrl-C restores the terminal.
    // SA_RESETHAND resets the disposition after the first delivery, so the
    // process terminates normally on a second Ctrl-C even if the first somehow
    // doesn't raise SIGINT.
    if (guard.active) {
        g_saved_for_signal = guard.saved;
        g_signal_active    = 1;

        struct sigaction sa{};
        sa.sa_handler = restore_echo_on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(SIGINT, &sa, nullptr);
    }

    std::cout << prompt_text << std::flush;
    std::string pass;
    std::getline(std::cin, pass);

    // Deactivate signal guard before EchoGuard destructor fires.
    g_signal_active = 0;

    if (guard.active) {
        std::cout << "\n" << std::flush; // print newline since echo was off
    }
    return pass;
#else
    // Windows: _getch reads char-by-char without echo.
    // Ctrl-C is handled by the console subsystem; no termios-equivalent risk.
    std::cout << prompt_text << std::flush;
    std::string pass;
    int ch;
    while ((ch = _getch()) != '\r' && ch != '\n' && ch != EOF) {
        if (ch == '\b') {
            // backspace
            if (!pass.empty()) {
                pass.pop_back();
            }
        } else {
            pass += static_cast<char>(ch);
        }
    }
    std::cout << "\n" << std::flush;
    return pass;
#endif
}

// Build an AuthService from the service container and TF_USER_EMAIL env var.
// If email is needed but unset, prompts interactively.
static std::shared_ptr<AuthService> make_auth_service(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets,
    std::string& email_out)
{
    const char* env_email = std::getenv("TF_USER_EMAIL");
    std::string email = (env_email && env_email[0] != '\0') ? std::string(env_email) : "";
    if (email.empty()) {
        email = prompt_line("Email: ");
    }
    email_out = email;
    return std::make_shared<AuthService>(backend, secrets, email);
}

// --------------------------------------------------------------------------
// CLI flag handlers
// --------------------------------------------------------------------------

// --enroll <token>
static int cmd_enroll(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets,
    const std::string& enroll_token)
{
    std::string email;
    auto auth = make_auth_service(backend, secrets, email);

    EnrollRequest req;
    req.token     = enroll_token;
    req.email     = email;
    req.passphrase = prompt_passphrase("Passphrase (new): ");

    std::string passphrase_confirm = prompt_passphrase("Passphrase (confirm): ");
    if (req.passphrase != passphrase_confirm) {
        sodium_memzero(req.passphrase.data(), req.passphrase.size());
        sodium_memzero(passphrase_confirm.data(), passphrase_confirm.size());
        std::cerr << "Error: passphrases do not match." << std::endl;
        return 1;
    }
    sodium_memzero(passphrase_confirm.data(), passphrase_confirm.size());

    // TOTP: server will generate one; the provisioning URI comes back.
    // No local TOTP secret is supplied here.

    auto result = auth->enroll(req);

    // Zeroize the passphrase from memory after use.
    sodium_memzero(req.passphrase.data(), req.passphrase.size());

    if (std::holds_alternative<BackendError>(result)) {
        const auto& err = std::get<BackendError>(result);
        std::cerr << "Enrollment failed: " << err.message
                  << " (code: " << err.code << ")" << std::endl;
        return 1;
    }

    const auto& er = std::get<EnrollResult>(result);
    std::cout << "Enrollment successful. User ID: " << er.user_id << "\n";
    std::cout << "TOTP provisioning URI (scan into your authenticator app):\n"
              << er.totp_provisioning_uri << "\n";
    std::cout << "Run --login to authenticate and cache your session." << std::endl;
    return 0;
}

// --login
static int cmd_login(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets)
{
    std::string email;
    auto auth = make_auth_service(backend, secrets, email);

    LoginRequest req;
    req.email     = email;
    req.passphrase = prompt_passphrase("Passphrase: ");
    req.totp_code  = prompt_line("TOTP code (6 digits): ");

    auto result = auth->login(req);

    // Zeroize sensitive fields after the call.
    sodium_memzero(req.passphrase.data(), req.passphrase.size());
    sodium_memzero(req.totp_code.data(), req.totp_code.size());

    if (std::holds_alternative<BackendError>(result)) {
        const auto& err = std::get<BackendError>(result);
        std::cerr << "Login failed: " << err.message
                  << " (code: " << err.code << ")" << std::endl;
        return 1;
    }

    const auto& lr = std::get<LoginResult>(result);
    std::cout << "Login successful. User ID: " << lr.user_id << "\n"
              << "Session expires at (unix): " << lr.expires_at_unix << std::endl;
    return 0;
}

// --logout
static int cmd_logout(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets)
{
    std::string email;
    auto auth = make_auth_service(backend, secrets, email);

    if (!auth->logout()) {
        std::cerr << "Logout failed (transport error). Session cache unchanged." << std::endl;
        return 1;
    }

    std::cout << "Logged out successfully." << std::endl;
    return 0;
}

// --whoami
static int cmd_whoami(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets)
{
    std::string email;
    auto auth = make_auth_service(backend, secrets, email);

    auto user_id = auth->current_user_id();
    if (!user_id.has_value()) {
        std::cout << "Not logged in." << std::endl;
        return 0;
    }
    std::cout << "Logged in as: " << *user_id << std::endl;
    return 0;
}

// --migrate-from-local <path>
static int cmd_migrate_from_local(
    std::shared_ptr<BackendClient> backend,
    std::shared_ptr<ISecretStore>  secrets,
    const std::string& json_path)
{
    // Require a logged-in session.
    const char* env_email = std::getenv("TF_USER_EMAIL");
    std::string email = (env_email && env_email[0] != '\0') ? std::string(env_email) : "";
    if (email.empty()) {
        std::cerr << "Error: TF_USER_EMAIL must be set for --migrate-from-local." << std::endl;
        return 1;
    }

    AuthService auth(backend, secrets, email);
    if (!auth.has_cached_session()) {
        std::cerr << "Run --login first." << std::endl;
        return 1;
    }

    // Retrieve the session token.
    // current_user_id() verifies the token is still valid (GET /auth/whoami).
    auto user_id = auth.current_user_id();
    if (!user_id.has_value()) {
        std::cerr << "Session expired or invalid. Run --login first." << std::endl;
        return 1;
    }

    // Read the cached token directly via the private API surface exposed by
    // AuthService. Since AuthService has no public get_token(), we re-read it
    // from the secret store using the same key format ("tf-session-{email}").
    // This is the minimal reach into internals required for the CLI path.
    std::optional<std::string> token_opt;
    {
        // Re-use the same ISecretStore that AuthService uses.
        std::string cache_key = "tf-session-" + email;
        auto raw = secrets->get(cache_key);
        if (raw.has_value()) {
            std::string tok;
            tok.reserve(raw->size());
            for (auto b : *raw) {
                tok += static_cast<char>(b);
            }
            token_opt = std::move(tok);
        }
    }

    if (!token_opt.has_value()) {
        std::cerr << "Could not read cached session token. Run --login first." << std::endl;
        return 1;
    }

    BackendClientAdapter adapter(*backend);
    V01Migrator migrator(adapter, *token_opt);

    std::cout << "Starting migration from: " << json_path << "\n";
    MigrationReport report = migrator.migrate(json_path);

    std::cout << "\n--- Migration Report ---\n";
    std::cout << "Entities:     " << report.entities_created     << " created, "
              << report.entities_skipped     << " skipped\n";
    std::cout << "Accounts:     " << report.accounts_created     << " created, "
              << report.accounts_skipped     << " skipped\n";
    std::cout << "Transactions: " << report.transactions_created << " created, "
              << report.transactions_skipped << " skipped\n";
    std::cout << "Categories:   " << report.categories_created   << " created, "
              << report.categories_skipped   << " skipped\n";
    std::cout << "Budgets:      " << report.budgets_created      << " created, "
              << report.budgets_skipped      << " skipped\n";
    std::cout << "Errors:       " << report.errors << "\n";

    if (!report.error_messages.empty()) {
        std::cout << "\nError details:\n";
        for (const auto& msg : report.error_messages) {
            std::cout << "  - " << msg << "\n";
        }
    }

    // Zeroize the session token from stack memory.
    sodium_memzero(token_opt->data(), token_opt->size());

    return (report.errors > 0) ? 1 : 0;
}

// --------------------------------------------------------------------------
// Shared service wiring (used by both CLI and TUI paths)
// --------------------------------------------------------------------------

struct CoreServices {
    std::shared_ptr<CurlHttpClient> http_client;
    std::shared_ptr<BackendClient>  backend;
    std::shared_ptr<ISecretStore>   secrets;
};

static CoreServices build_core_services() {
    CoreServices s;
    s.http_client = std::make_shared<CurlHttpClient>();

    const char* backend_url_env = std::getenv("TF_BACKEND_URL");
    std::string backend_url = (backend_url_env && backend_url_env[0] != '\0')
        ? std::string(backend_url_env)
        : "https://localhost:8443";
    try {
        s.backend = std::make_shared<BackendClient>(s.http_client, backend_url);
    } catch (const std::invalid_argument& e) {
        Logger::instance().warning(std::string("BackendClient not created: ") + e.what());
    }

#ifdef _WIN32
    s.secrets = std::make_shared<DpapiSecretStore>();
#elif defined(__APPLE__)
    s.secrets = std::make_shared<KeychainSecretStore>();
#else
    s.secrets = std::make_shared<FileSecretStore>();
#endif

    return s;
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------

int main(int argc, char** argv) {
    // libsodium initialization. Must happen before any crypto primitive is
    // used. Returns -1 on failure (fatal), 0 on first success, 1 if already
    // initialized. The crypto functions also self-init defensively, but
    // calling here makes the contract explicit and catches misconfiguration
    // at startup rather than first use.
    if (sodium_init() < 0) {
        std::cerr << "FATAL: sodium_init() failed; crypto unavailable" << std::endl;
        return 1;
    }

    // ------------------------------------------------------------------
    // CLI flag dispatch — must happen BEFORE the App/FTXUI initialisation
    // so non-TUI modes exit cleanly without starting a full-screen UI.
    // ------------------------------------------------------------------
    auto core = build_core_services();

    if (argc >= 2) {
        std::string_view flag(argv[1]);

        if (flag == "--enroll") {
            if (argc < 3) {
                std::cerr << "Usage: TerminalFinance --enroll <token>" << std::endl;
                return 1;
            }
            std::string enroll_token(argv[2]);
            if (!core.backend) {
                std::cerr << "Error: BackendClient not initialized (bad TF_BACKEND_URL?)" << std::endl;
                return 1;
            }
            if (!core.secrets) {
                std::cerr << "Error: SecretStore not available on this platform." << std::endl;
                return 1;
            }
            return cmd_enroll(core.backend, core.secrets, enroll_token);
        }

        if (flag == "--login") {
            if (!core.backend) {
                std::cerr << "Error: BackendClient not initialized (bad TF_BACKEND_URL?)" << std::endl;
                return 1;
            }
            if (!core.secrets) {
                std::cerr << "Error: SecretStore not available on this platform." << std::endl;
                return 1;
            }
            return cmd_login(core.backend, core.secrets);
        }

        if (flag == "--logout") {
            if (!core.backend) {
                std::cerr << "Error: BackendClient not initialized (bad TF_BACKEND_URL?)" << std::endl;
                return 1;
            }
            if (!core.secrets) {
                std::cerr << "Error: SecretStore not available on this platform." << std::endl;
                return 1;
            }
            return cmd_logout(core.backend, core.secrets);
        }

        if (flag == "--whoami") {
            if (!core.backend) {
                std::cerr << "Error: BackendClient not initialized (bad TF_BACKEND_URL?)" << std::endl;
                return 1;
            }
            if (!core.secrets) {
                std::cerr << "Error: SecretStore not available on this platform." << std::endl;
                return 1;
            }
            return cmd_whoami(core.backend, core.secrets);
        }

        if (flag == "--migrate-from-local") {
            if (argc < 3) {
                std::cerr << "Usage: TerminalFinance --migrate-from-local <path-to-v0.1-json>" << std::endl;
                return 1;
            }
            std::string migrate_path(argv[2]);
            if (!core.backend) {
                std::cerr << "Error: BackendClient not initialized (bad TF_BACKEND_URL?)" << std::endl;
                return 1;
            }
            if (!core.secrets) {
                std::cerr << "Error: SecretStore not available on this platform." << std::endl;
                return 1;
            }
            return cmd_migrate_from_local(core.backend, core.secrets, migrate_path);
        }

        std::cerr << "Unknown flag: " << flag << "\n"
                  << "Usage: TerminalFinance [--enroll <token>|--login|--logout|--whoami|--migrate-from-local <path>]"
                  << std::endl;
        return 1;
    }

    // ------------------------------------------------------------------
    // Default: launch the TUI. First verify the user has a valid session.
    // ------------------------------------------------------------------
    if (core.backend && core.secrets) {
        const char* env_email = std::getenv("TF_USER_EMAIL");
        std::string email = (env_email && env_email[0] != '\0') ? std::string(env_email) : "";
        if (!email.empty()) {
            AuthService auth_check(core.backend, core.secrets, email);
            auto user_id = auth_check.current_user_id();
            if (!user_id.has_value()) {
                std::cerr << "Please run with --login first." << std::endl;
                return 1;
            }
        }
        // RC-3: If TF_USER_EMAIL is unset, the auth gate is bypassed.
        // Log a visible warning so the operator notices in production logs.
        // This preserves v0.1 backward compatibility; v0.3 may make auth mandatory.
        if (email.empty()) {
            Logger::instance().warning(
                "TF_USER_EMAIL is not set; auth gate is bypassed. "
                "Run with TF_USER_EMAIL=<email> ./TerminalFinance to enable session-gated mode. "
                "v0.2 default falls back to v0.1 unauthenticated TUI behavior.");
        }
    }

    auto screen = ScreenInteractive::Fullscreen();

    App app;
    // Wire the graceful-exit closure so dispatch(Quit) / Esc-on-empty
    // can terminate the FTXUI loop from inside the App.
    app.on_exit_ = screen.ExitLoopClosure();

    auto component = Renderer([&] {
        return app.render();
    });

    component = CatchEvent(component, [&](Event event) {
        // -----------------------------------------------------------------
        // Task v0.3-4: modal layer is top priority.  If the command
        // palette or help overlay is open, route ALL events through it
        // first.  Esc inside a modal is consumed by the modal itself;
        // see CommandPalette::handle_key / HelpOverlay::handle_key.
        //
        // When the modal closes (handle_key returns true after consuming
        // Esc/Enter), we still propagate the focus controller's
        // exit_modal() so the App's level state stays in sync.  The
        // modal -> dispatch -> close chain calls close_modals() through
        // dispatch's path; here we mirror it for the Esc-cancel path.
        // -----------------------------------------------------------------
        if (app.palette_.is_open()) {
            const bool was_open = true;
            const bool consumed = app.palette_.handle_key(event);
            if (was_open && !app.palette_.is_open()) {
                app.focus_.exit_modal();
            }
            if (consumed) return true;
        }
        if (app.help_.is_open()) {
            const bool was_open = true;
            const bool consumed = app.help_.handle_key(event);
            if (was_open && !app.help_.is_open()) {
                app.focus_.exit_modal();
            }
            if (consumed) return true;
        }

        // -----------------------------------------------------------------
        // Task v0.3-4: global hotkeys ":" (palette) and "?" (help).
        // These open the modal layer; they MUST run before the focus
        // controller so a user pressing ":" on the Dashboard with a
        // widget focused doesn't accidentally route the colon through
        // hjkl (it wouldn't match, but the precedence is the
        // documented one -- Appendix C.3).
        // -----------------------------------------------------------------
        if (event == Event::Character(':')) {
            app.open_palette();
            return true;
        }
        if (event == Event::Character('?')) {
            app.open_help();
            return true;
        }

        // -----------------------------------------------------------------
        // Task v0.3-1: route Tab/Shift-Tab/hjkl/arrows/Esc through the
        // FocusController FIRST when the Dashboard view is active.  If
        // consumed, the legacy handlers below never see the event.
        //
        // Outside the Dashboard view we explicitly do NOT consult the
        // focus controller -- Tab/Shift-Tab there must continue to cycle
        // top-level views (existing v0.2 behavior) and the controller is
        // kept in a reset state.  The redesign §3a §"How focus moves"
        // documents this Dashboard-only carve-out.
        // -----------------------------------------------------------------
        if (app.current_tab == 0) {
            // Task v0.3-2: Enter on a focused widget drills into it.
            // Routed here BEFORE FocusController::handle_key() because
            // the controller does not own Enter semantics — only the
            // App knows which WidgetId is currently focused and whether
            // a drill view exists for it.
            //
            // Currently only NetWorth has a drill implementation;
            // Enter on the other widgets is a silent no-op until their
            // drills land.
            if (event == Event::Return &&
                app.focus_.level() == tf::views::FocusLevel::Widget) {
                const tf::views::WidgetId w = app.focus_.focused_widget();
                if (w == tf::views::WidgetId::NetWorth) {
                    app.focus_.enter_drill(w);
                }
                return true;
            }
            if (app.focus_.handle_key(event)) {
                return true;
            }
        }

        // Legacy Esc-to-exit (v0.2).  Preserved for non-Dashboard views.
        // On Dashboard, the focus controller swallows Esc (either Widget
        // -> Dashboard pop, or explicit no-op at Dashboard top-level per
        // Q3), so this branch is unreachable from current_tab == 0.
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

        // Q14: vim-style `g`+letter top-level navigation.
        //   g d → Dashboard, g a → Accounts, g t → Transactions, g b → Budget.
        // The first `g` arms a pending state; the next keypress either
        // matches one of the letters (and switches view) or clears the
        // state silently.  Tab no longer switches top-level views; the
        // palette (":" → "dashboard"/"accounts"/"tx"/"budget") remains
        // as a fallback for users who haven't learned `g`+letter yet.
        if (app.g_pending) {
            app.g_pending = false;
            if      (event == Event::Character('d')) { app.current_tab = 0; app.focus_.reset(); return true; }
            else if (event == Event::Character('a')) { app.current_tab = 1; app.focus_.reset(); return true; }
            else if (event == Event::Character('t')) { app.current_tab = 2; app.focus_.reset(); return true; }
            else if (event == Event::Character('b')) { app.current_tab = 3; app.focus_.reset(); return true; }
            // Any other key after `g` is just silently ignored — fall
            // through and let downstream handlers see the original event.
        }
        if (event == Event::Character('g')) {
            app.g_pending = true;
            app.status_message = "g- pending (d/a/t/b)";
            return true;
        }

        // Arrow navigation within views (non-Dashboard only -- arrows on
        // Dashboard are claimed by the focus controller above).
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
        // Link Plaid account (v0.2: server-mediated Plaid Link flow)
        if (event == Event::Character('l') || event == Event::Character('L')) {
            if (app.current_tab == 1) {
                auto plaid = app.services.get_plaid();
                if (!plaid) {
                    app.status_message = "Plaid service not available.";
                    return true;
                }
                auto entity_id = app.data_store.entities.empty()
                    ? "" : app.data_store.entities[app.current_entity].id;
                auto accounts = app.data_store.get_accounts_for_entity(entity_id);
                int selected = app.accounts_view->get_selected();
                if (selected >= 0 && selected < (int)accounts.size()) {
                    const auto& acc = *accounts[selected];
                    app.status_message = "Linking account '" + acc.name + "' via Plaid...";
                    bool ok = plaid->initiate_link_flow(acc.id);
                    if (ok) {
                        app.status_message = "Account '" + acc.name + "' linked successfully.";
                    } else {
                        app.status_message = "Link failed: " + plaid->get_last_error();
                    }
                } else {
                    app.status_message = "No account selected.";
                }
            } else {
                app.status_message = "Switch to Accounts view (Tab) to link an account.";
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