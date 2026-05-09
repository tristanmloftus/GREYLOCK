#pragma once

#include <string>
#include <optional>
#include <windows.h>
#include <wincred.h>

namespace SecurityService {
    bool encrypt_data(const std::string& plaintext, std::string& ciphertext);
    bool decrypt_data(const std::string& ciphertext, std::string& plaintext);
    bool store_access_token(const std::string& account_id, const std::string& token);
    std::optional<std::string> retrieve_access_token(const std::string& account_id);
    bool delete_access_token(const std::string& account_id);
}