#include "FileSecretStore.h"

#include <sodium.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <sys/stat.h>
#include <sys/types.h>

namespace fs = std::filesystem;

namespace {

fs::path config_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        return fs::path(xdg) / "TerminalFinance" / "secrets";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return fs::path(home) / ".config" / "TerminalFinance" / "secrets";
    }
    // Last-resort fallback: cwd-relative. Production hosts should have HOME set.
    return fs::path(".terminalfinance-secrets");
}

void ensure_dir_700(const fs::path& d) {
    std::error_code ec;
    fs::create_directories(d, ec);
    // Always chmod (in case the dir pre-existed with looser perms).
    ::chmod(d.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
}

// BLAKE2b-256 of the key, hex-encoded, plus a .bin extension. Keeps the
// filename stable, filesystem-safe, and avoids leaking the cleartext key in
// `ls` output.
std::string key_to_filename(std::string_view key) {
    unsigned char out[crypto_generichash_BYTES]; // 32 bytes
    int rc = crypto_generichash(
        out, sizeof(out),
        reinterpret_cast<const unsigned char*>(key.data()), key.size(),
        nullptr, 0);
    if (rc != 0) {
        // Generichash never fails for these arg shapes, but fail loud if it does.
        return std::string("invalid_") + std::to_string(rc) + ".bin";
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : out) {
        oss << std::setw(2) << static_cast<unsigned>(c);
    }
    oss << ".bin";
    return oss.str();
}

} // namespace

FileSecretStore::FileSecretStore() : base_dir_(config_dir()) {
    ensure_dir_700(base_dir_);
}

bool FileSecretStore::put(std::string_view key, std::span<const std::byte> value) {
    ensure_dir_700(base_dir_);
    fs::path target = base_dir_ / key_to_filename(key);

    // Write to a temp file then atomic-rename into place — avoids partial
    // writes leaving a half-written secret on disk.
    fs::path tmp = target;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        if (!value.empty()) {
            f.write(reinterpret_cast<const char*>(value.data()),
                    static_cast<std::streamsize>(value.size()));
        }
        f.close();
        if (!f.good()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }

    // Tighten perms BEFORE rename so the final filename never exists with looser
    // perms even briefly.
    if (::chmod(tmp.string().c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> FileSecretStore::get(std::string_view key) {
    fs::path source = base_dir_ / key_to_filename(key);
    std::error_code ec;
    if (!fs::exists(source, ec)) return std::nullopt;

    std::ifstream f(source, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return std::nullopt;

    std::streamsize size = f.tellg();
    if (size < 0) return std::nullopt;
    f.seekg(0, std::ios::beg);

    std::vector<std::byte> buf(static_cast<size_t>(size));
    if (size > 0) {
        if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
            return std::nullopt;
        }
    }
    return buf;
}

bool FileSecretStore::remove(std::string_view key) {
    fs::path target = base_dir_ / key_to_filename(key);
    std::error_code ec;
    // remove() returns true if removed, false if file didn't exist; both are
    // success for our API contract.
    fs::remove(target, ec);
    return !ec || ec == std::errc::no_such_file_or_directory;
}
