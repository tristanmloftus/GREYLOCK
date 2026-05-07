// Greylock — KDF wrappers
// =============================================================================
// Thin, paranoia-checked wrappers around Node's built-in KDFs. Internal helpers
// throw on parameter violations; module-boundary functions in `index.ts` catch
// and convert to `Result<_, CryptoError>`.
//
// No homemade crypto. No `pseudoRandomBytes`. No external libs.
// =============================================================================

import { hkdfSync, scryptSync } from 'node:crypto';

// -----------------------------------------------------------------------------
// HKDF-SHA-256
// -----------------------------------------------------------------------------

export interface HkdfInput {
  readonly ikm: Uint8Array;
  readonly salt: Uint8Array;
  readonly info: Uint8Array;
  readonly length: number;
}

/**
 * HKDF-SHA-256 with strict parameter validation.
 *
 * Throws `Error` (caller catches at module boundary) on:
 *   - empty IKM (HKDF allows it but we never want it for our use cases)
 *   - non-positive length
 *   - length > 8160 (RFC 5869 max for SHA-256: 255 * HashLen = 255 * 32)
 *
 * Returns a freshly allocated `Buffer` of `length` bytes. Caller is responsible
 * for zeroizing the buffer after use.
 */
export function hkdf(input: HkdfInput): Buffer {
  validateHkdf(input);
  const { ikm, salt, info, length } = input;
  // hkdfSync returns ArrayBuffer; wrap in Buffer for ergonomics + .fill(0).
  const out = hkdfSync('sha256', ikm, salt, info, length);
  return Buffer.from(out);
}

function validateHkdf(input: HkdfInput): void {
  if (input.ikm.byteLength === 0) {
    throw new Error('hkdf: ikm must be non-empty');
  }
  if (!Number.isInteger(input.length) || input.length <= 0) {
    throw new Error('hkdf: length must be a positive integer');
  }
  // RFC 5869 §2.3 max output for SHA-256 = 255 * 32 = 8160 bytes.
  if (input.length > 8160) {
    throw new Error('hkdf: length exceeds RFC 5869 max for SHA-256');
  }
}

// -----------------------------------------------------------------------------
// scrypt
// -----------------------------------------------------------------------------

export interface ScryptInput {
  readonly password: Uint8Array;
  readonly salt: Uint8Array;
  readonly N: number;
  readonly r: number;
  readonly p: number;
  readonly length: number;
}

/**
 * scrypt KDF with strict parameter validation. Used only for the Master KEK
 * derivation from the master passphrase (ARCHITECTURE.md §3, scrypt N=2^17,
 * r=8, p=1, dkLen=32).
 *
 * Throws on invalid parameters. Caller catches at module boundary.
 *
 * Memory cost is the responsibility of the caller; Node's default
 * `maxmem = 32 MiB` is too small for N=2^17 (cost ≈ 128*N*r = 128 MiB), so
 * we pass `maxmem = 256 MiB` explicitly.
 */
export function scrypt(input: ScryptInput): Buffer {
  validateScrypt(input);
  const { password, salt, N, r, p, length } = input;
  const out = scryptSync(password, salt, length, {
    N,
    r,
    p,
    maxmem: 256 * 1024 * 1024,
  });
  return Buffer.from(out);
}

function validateScrypt(input: ScryptInput): void {
  if (input.password.byteLength === 0) {
    throw new Error('scrypt: password must be non-empty');
  }
  if (input.salt.byteLength === 0) {
    throw new Error('scrypt: salt must be non-empty');
  }
  if (!Number.isInteger(input.length) || input.length <= 0) {
    throw new Error('scrypt: length must be a positive integer');
  }
  if (!Number.isInteger(input.N) || input.N < 2 || (input.N & (input.N - 1)) !== 0) {
    throw new Error('scrypt: N must be a power of 2 and >= 2');
  }
  if (!Number.isInteger(input.r) || input.r <= 0) {
    throw new Error('scrypt: r must be a positive integer');
  }
  if (!Number.isInteger(input.p) || input.p <= 0) {
    throw new Error('scrypt: p must be a positive integer');
  }
}

// -----------------------------------------------------------------------------
// Locked parameters per ARCHITECTURE.md §3 / SPEC §3.
// Exported so call-sites (master-key.ts, user-dek.ts) reference the same
// constants and tests can assert byte-exact behavior.
// -----------------------------------------------------------------------------

export const SCRYPT_PARAMS = Object.freeze({
  N: 1 << 17,
  r: 8,
  p: 1,
  length: 32,
});

export const HKDF_USER_KEK_INFO_PREFIX = 'greylock/userKek/v1/';
export const HKDF_KEYBRIDGE_INFO = 'greylock/keybridge/v1';
export const KEY_LENGTH_BYTES = 32;
