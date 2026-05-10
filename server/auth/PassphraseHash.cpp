#include "PassphraseHash.h"

#include <sodium.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf::auth {

// ---------------------------------------------------------------------------
// hash_passphrase
// ---------------------------------------------------------------------------
// Argon2id parameters (tunable in Phase 6):
//   OPSLIMIT_MODERATE = 3 operations (CPU cost)
//   MEMLIMIT_MODERATE = 256 MiB (memory cost)
//
// We use crypto_pwhash_str (not raw crypto_pwhash) so the output is a
// self-describing ASCII string that embeds the algorithm, salt, opslimit,
// and memlimit.  This means verify_passphrase can always reconstruct the
// right parameters even if we tune them in a future release.
//
// The output vector is exactly crypto_pwhash_STRBYTES bytes.  The first
// crypto_pwhash_STRBYTES - 1 bytes are the encoded ASCII string;
// the final byte is the NUL terminator that crypto_pwhash_str writes.
// Storing this NUL terminator is intentional: verify() casts the stored
// bytes directly to char* for crypto_pwhash_str_verify.
// ---------------------------------------------------------------------------
std::vector<std::byte> hash_passphrase(std::string_view passphrase) {
    std::vector<std::byte> out(crypto_pwhash_STRBYTES, std::byte{0});

    int rc = crypto_pwhash_str(
        reinterpret_cast<char*>(out.data()),
        passphrase.data(),
        passphrase.size(),
        crypto_pwhash_OPSLIMIT_MODERATE,
        crypto_pwhash_MEMLIMIT_MODERATE
    );

    if (rc != 0) {
        throw std::runtime_error(
            "PassphraseHash::hash_passphrase: crypto_pwhash_str failed "
            "(out-of-memory or libsodium not initialized)");
    }

    return out;
}

// ---------------------------------------------------------------------------
// verify_passphrase
// ---------------------------------------------------------------------------
// Delegates entirely to crypto_pwhash_str_verify which is documented by
// libsodium as constant-time.  Returns true on match, false on any failure
// including malformed hash blobs.
// ---------------------------------------------------------------------------
bool verify_passphrase(std::string_view passphrase,
                       std::span<const std::byte> stored_hash) {
    if (stored_hash.size() < crypto_pwhash_STRBYTES) {
        return false;
    }

    int rc = crypto_pwhash_str_verify(
        reinterpret_cast<const char*>(stored_hash.data()),
        passphrase.data(),
        passphrase.size()
    );

    return rc == 0;
}

} // namespace tf::auth
