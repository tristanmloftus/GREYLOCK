#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Cross-platform interface for OS-managed secret storage.
// On Windows: DpapiSecretStore (DPAPI + HKCU registry).
// On macOS:   KeychainSecretStore (Security.framework kSecClassGenericPassword).
// Selection is made at CMake target level; only one implementation is compiled per platform.
class ISecretStore {
public:
    virtual ~ISecretStore() = default;

    // Store 'value' under 'key'. Returns true on success.
    virtual bool put(std::string_view key, std::span<const std::byte> value) = 0;

    // Retrieve the value for 'key'. Returns std::nullopt if not found or on error.
    virtual std::optional<std::vector<std::byte>> get(std::string_view key) = 0;

    // Delete the entry for 'key'. Returns true on success (including key-not-found).
    virtual bool remove(std::string_view key) = 0;
};
