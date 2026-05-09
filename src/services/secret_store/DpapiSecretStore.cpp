// Windows-only. Only compiled when _WIN32 is defined (CMake enforces this).
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <vector>

#include "DpapiSecretStore.h"
#include "../../utils/Logger.h"

bool DpapiSecretStore::put(std::string_view key, std::span<const std::byte> value) {
    // Encrypt the caller-supplied bytes with DPAPI (per-user, no UI prompt).
    DATA_BLOB input;
    // CryptProtectData takes a non-const BYTE* even though it doesn't write to it.
    input.pbData = reinterpret_cast<BYTE*>(const_cast<std::byte*>(value.data()));
    input.cbData = static_cast<DWORD>(value.size());

    DATA_BLOB output = {0};
    if (!CryptProtectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        Logger::instance().error("DpapiSecretStore: CryptProtectData failed");
        return false;
    }

    // Persist the ciphertext into HKCU\Software\TerminalFinance\Tokens.
    HKEY hKey;
    DWORD disp;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRegistrySubkey, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disp) != ERROR_SUCCESS) {
        LocalFree(output.pbData);
        Logger::instance().error("DpapiSecretStore: Failed to create/open registry key");
        return false;
    }

    std::string key_str(key);
    LONG rc = RegSetValueExA(hKey, key_str.c_str(), 0, REG_BINARY,
        output.pbData, output.cbData);
    LocalFree(output.pbData);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS) {
        Logger::instance().error("DpapiSecretStore: RegSetValueExA failed for key: " + key_str);
        return false;
    }

    Logger::instance().info("DpapiSecretStore: Stored secret for key: " + key_str);
    return true;
}

std::optional<std::vector<std::byte>> DpapiSecretStore::get(std::string_view key) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRegistrySubkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        Logger::instance().debug("DpapiSecretStore: Registry key not found (no secrets stored yet)");
        return std::nullopt;
    }

    std::string key_str(key);
    DWORD size = 0;
    RegQueryValueExA(hKey, key_str.c_str(), NULL, NULL, NULL, &size);
    if (size == 0) {
        RegCloseKey(hKey);
        return std::nullopt;
    }

    std::vector<BYTE> encrypted(size);
    if (RegQueryValueExA(hKey, key_str.c_str(), NULL, NULL,
            encrypted.data(), &size) != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return std::nullopt;
    }
    RegCloseKey(hKey);

    // Decrypt with DPAPI.
    DATA_BLOB input;
    input.pbData = encrypted.data();
    input.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB output = {0};
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        Logger::instance().error("DpapiSecretStore: CryptUnprotectData failed for key: " + key_str);
        return std::nullopt;
    }

    std::vector<std::byte> result(output.cbData);
    std::memcpy(result.data(), output.pbData, output.cbData);
    LocalFree(output.pbData);

    Logger::instance().debug("DpapiSecretStore: Retrieved secret for key: " + key_str);
    return result;
}

bool DpapiSecretStore::remove(std::string_view key) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRegistrySubkey, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        // Registry subkey doesn't exist yet; the value is already absent. Treat as success.
        return true;
    }

    std::string key_str(key);
    // Security review (Phase 0 audit): the original code discarded RegDeleteValueA's return
    // value, silently swallowing genuine I/O failures. Now we check it.
    // ERROR_FILE_NOT_FOUND means the value was already absent — idempotent success.
    // Any other non-zero code is a real failure; log it and return false.
    LONG rc = RegDeleteValueA(hKey, key_str.c_str());
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        Logger::instance().error("DpapiSecretStore: RegDeleteValueA failed for key: " + key_str +
            " (LONG=" + std::to_string(rc) + ")");
        return false;
    }

    Logger::instance().info("DpapiSecretStore: Removed secret for key: " + key_str);
    return true;
}

#endif // _WIN32
