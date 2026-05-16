#pragma once

// FileSecretStore — cross-platform ISecretStore backed by a mode-600 file
// per secret in a mode-700 directory.
//
// Used as the default ISecretStore implementation on Linux + any other POSIX
// platform where DPAPI / Keychain are unavailable. Filesystem permissions are
// the authorization layer (same model as ~/.ssh/id_*).
//
// Layout: $XDG_CONFIG_HOME/TerminalFinance/secrets/<blake2b256-hex>.bin
//   (falls back to $HOME/.config/TerminalFinance/secrets/ if XDG_CONFIG_HOME unset)
//
// Each secret value is stored as raw bytes. Keys are hashed (BLAKE2b-256) to
// produce stable filename-safe identifiers without leaking the key text in
// directory listings.
//
// Threat model:
//   - Other unprivileged users on the same machine: cannot read (file mode 600,
//     dir mode 700).
//   - Privileged users (root): can read. Same as Keychain / DPAPI under root.
//   - Filesystem snapshots / backups: contents are plaintext. Disk-level
//     encryption (LUKS, FileVault, BitLocker) is the at-rest defense.
//
// Suitable for headless Linux hosts (no DBus / no gnome-keyring) where the
// libsecret approach would not work.

#include "../ISecretStore.h"

#include <filesystem>

class FileSecretStore : public ISecretStore {
public:
    FileSecretStore();

    bool put(std::string_view key, std::span<const std::byte> value) override;
    std::optional<std::vector<std::byte>> get(std::string_view key) override;
    bool remove(std::string_view key) override;

private:
    std::filesystem::path base_dir_;
};
