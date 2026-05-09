#include "SecurityService.h"
#include <vector>
#include <windows.h>
#include "../utils/Logger.h"

namespace SecurityService {
    bool encrypt_data(const std::string& plaintext, std::string& ciphertext) {
        DATA_BLOB input;
        input.pbData = (BYTE*)plaintext.data();
        input.cbData = static_cast<DWORD>(plaintext.size());

        DATA_BLOB output = {0};

        if (!CryptProtectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            Logger::instance().error("SecurityService: CryptProtectData failed");
            return false;
        }

        ciphertext.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);

        Logger::instance().debug("SecurityService: Data encrypted successfully");
        return true;
    }

    bool decrypt_data(const std::string& ciphertext, std::string& plaintext) {
        DATA_BLOB input;
        input.pbData = (BYTE*)ciphertext.data();
        input.cbData = static_cast<DWORD>(ciphertext.size());

        DATA_BLOB output = {0};

        if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            Logger::instance().error("SecurityService: CryptUnprotectData failed");
            return false;
        }

        plaintext.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);

        Logger::instance().debug("SecurityService: Data decrypted successfully");
        return true;
    }

    bool store_access_token(const std::string& account_id, const std::string& token) {
        std::string encrypted;
        if (!encrypt_data(token, encrypted)) {
            return false;
        }

        HKEY hKey;
        std::string subkey = "Software\\TerminalFinance\\Tokens";
        DWORD disp;
        
        if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey.c_str(), 0, NULL, 
            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &disp) != ERROR_SUCCESS) {
            Logger::instance().error("SecurityService: Failed to create registry key");
            return false;
        }

        RegSetValueExA(hKey, account_id.c_str(), 0, REG_BINARY, 
            (const BYTE*)encrypted.data(), static_cast<DWORD>(encrypted.size()));
        RegCloseKey(hKey);

        Logger::instance().info("SecurityService: Stored access token for account: " + account_id);
        return true;
    }

    std::optional<std::string> retrieve_access_token(const std::string& account_id) {
        HKEY hKey;
        std::string subkey = "Software\\TerminalFinance\\Tokens";
        
        if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            Logger::instance().warning("SecurityService: No token found for account: " + account_id);
            return std::nullopt;
        }

        DWORD size = 0;
        RegQueryValueExA(hKey, account_id.c_str(), NULL, NULL, NULL, &size);
        
        if (size == 0) {
            RegCloseKey(hKey);
            return std::nullopt;
        }

        std::string encrypted(size, 0);
        RegQueryValueExA(hKey, account_id.c_str(), NULL, NULL, (BYTE*)encrypted.data(), &size);
        RegCloseKey(hKey);

        std::string decrypted;
        if (!decrypt_data(encrypted, decrypted)) {
            return std::nullopt;
        }

        Logger::instance().debug("SecurityService: Retrieved access token for account: " + account_id);
        return decrypted;
    }

    bool delete_access_token(const std::string& account_id) {
        HKEY hKey;
        std::string subkey = "Software\\TerminalFinance\\Tokens";
        
        if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
            Logger::instance().warning("SecurityService: Failed to open registry key");
            return false;
        }

        RegDeleteValueA(hKey, account_id.c_str());
        RegCloseKey(hKey);

        Logger::instance().info("SecurityService: Deleted access token for account: " + account_id);
        return true;
    }
}