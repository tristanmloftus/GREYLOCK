#pragma once

// KeychainSecretStore is only compiled on Apple platforms. This header should
// only be included from Apple-specific translation units or guarded with #ifdef __APPLE__.
#ifdef __APPLE__

#include "../ISecretStore.h"

// macOS implementation of ISecretStore.
// Uses Security.framework SecItemAdd / SecItemCopyMatching / SecItemDelete
// against kSecClassGenericPassword, service name "com.terminalfinance.secrets".
//
// All secrets are stored in the user's login keychain under the service
// "com.terminalfinance.secrets". The 'key' argument maps to kSecAttrAccount.
// No access-group is set, so the item is accessible only to this process's
// bundle (or to any process on macOS if running without a bundle/entitlements,
// as is typical for a CLI tool built without code signing).
//
// Access control: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly — the
// keychain item is readable after the user's first login post-reboot, and is
// NOT backed up to iCloud or migrated to other devices.
class KeychainSecretStore : public ISecretStore {
public:
    KeychainSecretStore() = default;
    ~KeychainSecretStore() override = default;

    bool put(std::string_view key, std::span<const std::byte> value) override;
    std::optional<std::vector<std::byte>> get(std::string_view key) override;
    bool remove(std::string_view key) override;

private:
    static constexpr const char* kServiceName = "com.terminalfinance.secrets";
};

#endif // __APPLE__
