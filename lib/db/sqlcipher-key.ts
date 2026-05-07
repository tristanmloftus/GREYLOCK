// Greylock — SQLCipher key derivation
// =============================================================================
// AGENT-DB. Derives the SQLCipher database-encryption key from the in-memory
// Master KEK using HKDF-SHA-256.
//
// The Master KEK is provided by the caller as raw bytes (32-byte secret). We
// do NOT touch the master passphrase, the Keychain, the env, or any other
// source of secrets here. The KEK is always supplied via parameter.
//
// The derived 32-byte key is fed to SQLCipher via `PRAGMA hexkey = '<hex>'`
// in `lib/db/client.ts`. Hex is used (not raw key string) so the key bytes
// are unambiguous and not subject to UTF-8 interpretation by SQLite.
//
// HKDF binding (locked):
//   IKM    = masterKek           (32 bytes)
//   salt   = empty                (per RFC 5869 fallback to zeros)
//   info   = utf8("greylock/sqlcipher-key/v1")
//   length = 32 bytes
//
// Versioned info string ("/v1") so that key-rotation flows have a clear
// upgrade path; bump to /v2 if/when the binding is changed.
// =============================================================================

import { hkdf, KEY_LENGTH_BYTES } from '../crypto/kdf.js';

const SQLCIPHER_INFO = 'greylock/sqlcipher-key/v1';
const SQLCIPHER_INFO_BYTES: Uint8Array = Buffer.from(SQLCIPHER_INFO, 'utf8');

// HKDF-RFC-5869 §2.2: salt MAY be empty; when empty, "a string of HashLen zeros
// is used". We pass an empty Uint8Array; node's hkdfSync handles the zero-pad.
const EMPTY_SALT: Uint8Array = new Uint8Array(0);

/**
 * Derive the 32-byte SQLCipher database-encryption key from a master KEK.
 *
 * @param masterKek 32-byte Master KEK held in process memory.
 * @returns 32-byte SQLCipher key. Caller MUST `Buffer.fill(0)` the buffer
 *          when the database connection is no longer needed.
 *
 * Throws if `masterKek` is not 32 bytes.
 */
export function deriveSqlcipherKey(masterKek: Uint8Array): Buffer {
  if (masterKek.byteLength !== KEY_LENGTH_BYTES) {
    throw new Error(
      `deriveSqlcipherKey: masterKek must be exactly ${String(KEY_LENGTH_BYTES)} bytes (got ${String(masterKek.byteLength)})`,
    );
  }
  return hkdf({
    ikm: masterKek,
    salt: EMPTY_SALT,
    info: SQLCIPHER_INFO_BYTES,
    length: KEY_LENGTH_BYTES,
  });
}

/**
 * Encode the 32-byte SQLCipher key as lowercase hex for use in
 * `PRAGMA hexkey = '<hex>'`. Returns a fresh string; caller is responsible
 * for ensuring the original key buffer is zeroized when no longer needed.
 *
 * Hex is used instead of the raw `key` pragma to avoid any string-encoding
 * ambiguity in the key material when passed through the SQL parser.
 */
export function sqlcipherKeyAsHex(key: Uint8Array): string {
  if (key.byteLength !== KEY_LENGTH_BYTES) {
    throw new Error(
      `sqlcipherKeyAsHex: key must be exactly ${String(KEY_LENGTH_BYTES)} bytes (got ${String(key.byteLength)})`,
    );
  }
  return Buffer.from(key).toString('hex');
}

/**
 * Public for tests/audits: the exact info string we pass to HKDF.
 * This is the cryptographic anchor — changing it changes every database key.
 */
export const SQLCIPHER_KEY_HKDF_INFO = SQLCIPHER_INFO;
