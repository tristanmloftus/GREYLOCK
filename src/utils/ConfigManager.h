#pragma once

#include <string>
#include <optional>
#include <map>

class ConfigManager {
public:
    static ConfigManager& instance();

    bool load_env_file(const std::string& filepath = ".env");
    std::optional<std::string> get(const std::string& key) const;
    std::optional<std::string> get_plaid_client_id() const;
    std::optional<std::string> get_plaid_secret() const;
    std::optional<std::string> get_plaid_environment() const;
    bool has_plaid_credentials() const;
    bool has_environment_config() const { return env_configured_; }

    std::string get_storage_path() const;
    void set_storage_path(const std::string& path) { storage_path_ = path; }

private:
    ConfigManager() = default;
    std::map<std::string, std::string> config_;
    bool env_configured_ = false;
    std::string storage_path_;
    bool load_from_environment();
};