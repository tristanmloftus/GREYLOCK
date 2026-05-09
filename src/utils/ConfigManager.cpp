#include "ConfigManager.h"
#include "Logger.h"
#include <fstream>
#include <sstream>

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load_env_file(const std::string& filepath) {
    config_.clear();
    storage_path_ = "data.json";

    if (!load_from_environment()) {
        Logger::instance().info("ConfigManager: No environment variables found, trying .env file");
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::instance().warning("ConfigManager: .env file not found, using environment variables");
        env_configured_ = !config_.empty();
        return env_configured_;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
                value.pop_back();
            }

            if (!value.empty() && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }

            if (key == "STORAGE_PATH") {
                storage_path_ = value;
            }

            config_[key] = value;
        }
    }

    env_configured_ = true;
    Logger::instance().info("ConfigManager: Loaded " + std::to_string(config_.size()) + " config entries");
    Logger::instance().info("ConfigManager: Storage path set to '" + storage_path_ + "'");
    return true;
}

bool ConfigManager::load_from_environment() {
    const char* env_vars[] = {"PLAID_CLIENT_ID", "PLAID_SECRET", "PLAID_ENVIRONMENT", "STORAGE_PATH"};
    bool found = false;

    for (const char* var : env_vars) {
        const char* value = std::getenv(var);
        if (value != nullptr) {
            config_[var] = value;
            if (var == std::string("STORAGE_PATH")) {
                storage_path_ = value;
            }
            found = true;
        }
    }

    return found;
}

std::optional<std::string> ConfigManager::get(const std::string& key) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> ConfigManager::get_plaid_client_id() const {
    auto it = config_.find("PLAID_CLIENT_ID");
    if (it != config_.end()) return it->second;

    const char* val = std::getenv("PLAID_CLIENT_ID");
    return val ? std::optional<std::string>(val) : std::nullopt;
}

std::optional<std::string> ConfigManager::get_plaid_secret() const {
    auto it = config_.find("PLAID_SECRET");
    if (it != config_.end()) return it->second;

    const char* val = std::getenv("PLAID_SECRET");
    return val ? std::optional<std::string>(val) : std::nullopt;
}

std::optional<std::string> ConfigManager::get_plaid_environment() const {
    auto it = config_.find("PLAID_ENVIRONMENT");
    if (it != config_.end()) return it->second;

    const char* val = std::getenv("PLAID_ENVIRONMENT");
    return val ? std::optional<std::string>(val) : std::nullopt;
}

bool ConfigManager::has_plaid_credentials() const {
    return get_plaid_client_id().has_value() && get_plaid_secret().has_value();
}

std::string ConfigManager::get_storage_path() const {
    return storage_path_.empty() ? "data.json" : storage_path_;
}