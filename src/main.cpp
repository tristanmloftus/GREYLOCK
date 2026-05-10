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

        bool use_stub = true;
        if (!client_id.empty() && !secret.empty()) {
            use_stub = false;
        }
        auto plaid = create_plaid_service(use_stub);
        if (use_stub) {
            Logger::instance().warning("Using stub Plaid service - no credentials configured");
        }
        services.set_plaid(plaid);

        // Wire platform-appropriate secret store.
#ifdef _WIN32
        services.set_secret_store(std::make_shared<DpapiSecretStore>());
#elif defined(__APPLE__)
        services.set_secret_store(std::make_shared<KeychainSecretStore>());
#endif

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
        // Link Plaid account (v0.2: server-mediated via PlaidTokenBroker)
        if (event == Event::Character('p') || event == Event::Character('P')) {
            app.status_message = "Plaid: Use 'L' to link an account via the server endpoint.";
            return true;
        }
        // Link with public token (v0.2: pass account_id + public_token to server)
        if (event == Event::Character('l') || event == Event::Character('L')) {
            auto plaid = app.services.get_plaid();
            if (plaid && !app.data_store.entities.empty() &&
                !app.data_store.accounts.empty()) {
                const auto& acc = app.data_store.accounts[0];
                bool ok = plaid->link_account(acc.id, "public-sandbox-test");
                if (ok) {
                    app.status_message = "Account linked via server.";
                } else {
                    app.status_message = "Link failed: " + plaid->get_last_error();
                }
            } else {
                app.status_message = "No accounts to link.";
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