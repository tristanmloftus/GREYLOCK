// Greylock — dev/test SQLCipher key derivation
// =============================================================================
// AGENT-DB. Dev-only helper: derive a SQLCipher key from a fixed-development
// passphrase set via the `DEV_DB_PASSPHRASE` env var. Used by tests and the
// `pnpm prisma migrate dev` flow to bootstrap an encrypted DB without
// touching the macOS Keychain.
//
// HARD GUARD: refuses to run when `NODE_ENV === 'production'`. The Master
// passphrase in production comes from Keychain via lib/crypto/master-key.ts.
//
// Derivation chain (matches production semantics):
//   1. fakeMasterKek = scrypt(DEV_DB_PASSPHRASE, salt='greylock-dev-salt', N=2^14)
//      (lower N than production — dev only — but same algorithm so the bytes
//       feeding HKDF are the right shape).
//   2. sqlcipherKey = HKDF-SHA-256(fakeMasterKek, info='greylock/sqlcipher-key/v1')
//
// The returned key is suitable for `createDbClient({ sqlcipherKey })`.
// =============================================================================

import { hkdfSync, scryptSync } from 'node:crypto';

import { deriveSqlcipherKey } from '../../lib/db/sqlcipher-key.js';

const DEV_SCRYPT_SALT = Buffer.from('greylock-dev-salt-v1', 'utf8');
const DEV_SCRYPT_N = 1 << 14; // dev — production uses 2^17
const DEV_SCRYPT_R = 8;
const DEV_SCRYPT_P = 1;

export interface DevKeyOutput {
  /** 32-byte key for SQLCipher. */
  readonly sqlcipherKey: Uint8Array;
  /** 32-byte fake master KEK. Some tests need it directly. */
  readonly fakeMasterKek: Uint8Array;
}

/**
 * Derive a deterministic dev SQLCipher key from `DEV_DB_PASSPHRASE`.
 *
 * Throws if:
 *   - `process.env.NODE_ENV === 'production'`,
 *   - `DEV_DB_PASSPHRASE` is missing or empty.
 */
export function deriveDevKey(opts?: { readonly passphrase?: string }): DevKeyOutput {
  if (process.env['NODE_ENV'] === 'production') {
    throw new Error('scripts/db/dev-key: refuse to derive a dev key in production NODE_ENV');
  }
  const passphrase = opts?.passphrase ?? process.env['DEV_DB_PASSPHRASE'] ?? '';
  if (passphrase.length === 0) {
    throw new Error(
      'scripts/db/dev-key: DEV_DB_PASSPHRASE env var is required (set it for tests / dev migrations)',
    );
  }
  const fakeMasterKek = scryptSync(Buffer.from(passphrase, 'utf8'), DEV_SCRYPT_SALT, 32, {
    N: DEV_SCRYPT_N,
    r: DEV_SCRYPT_R,
    p: DEV_SCRYPT_P,
    maxmem: 64 * 1024 * 1024,
  });
  const sqlcipherKey = deriveSqlcipherKey(fakeMasterKek);
  return {
    sqlcipherKey: new Uint8Array(sqlcipherKey),
    fakeMasterKek: new Uint8Array(fakeMasterKek),
  };
}

/** Print the SQLCipher key as hex on stdout. Used by the migration flow if
 *  we need to feed an external tool that takes a raw hex key. */
export function printSqlcipherKeyHex(): void {
  const { sqlcipherKey } = deriveDevKey();
   
  process.stdout.write(Buffer.from(sqlcipherKey).toString('hex') + '\n');
}

// Suppress unused-import warning for hkdfSync (we consume it indirectly via
// `deriveSqlcipherKey`).
export const __DEV_KEY_KDF__ = { hkdfSync };

if (process.argv[1]?.endsWith('dev-key.ts') || process.argv[1]?.endsWith('dev-key.js')) {
  // CLI invocation: `tsx scripts/db/dev-key.ts hex` prints the hex key.
  if (process.argv[2] === 'hex') {
    printSqlcipherKeyHex();
  } else {
    process.stdout.write(
      'usage: tsx scripts/db/dev-key.ts hex\n  prints the dev SQLCipher key as hex.\n',
    );
  }
}
