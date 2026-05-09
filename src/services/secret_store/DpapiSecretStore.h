#pragma once

// DpapiSecretStore is only compiled on Windows. This header should only be
// included from Windows-specific translation units or guarded with #ifdef _WIN32.
#ifdef _WIN32

#include "../ISecretStore.h"

// Windows implementation of ISecretStore.
// Uses DPAPI (CryptProtectData / CryptUnprotectData) for envelope encryption
// and stores ciphertexts in HKCU\Software\TerminalFinance\Tokens, keyed by
// the 'key' argument passed to put/get/remove.
//
// Behavior is identical to the v0.1 SecurityService: per-user DPAPI binding,
// CRYPTPROTECT_UI_FORBIDDEN flag, registry path Software\TerminalFinance\Tokens.
class DpapiSecretStore : public ISecretStore {
public:
    DpapiSecretStore() = default;
    ~DpapiSecretStore() override = default;

    bool put(std::string_view key, std::span<const std::byte> value) override;
    std::optional<std::vector<std::byte>> get(std::string_view key) override;
    bool remove(std::string_view key) override;

private:
    static constexpr const char* kRegistrySubkey = "Software\\TerminalFinance\\Tokens";
};

#endif // _WIN32
