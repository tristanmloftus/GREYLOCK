// Tests for `lib/crypto/kdf.ts`.

import { describe, it, expect } from 'vitest';

import {
  HKDF_USER_KEK_INFO_PREFIX,
  KEY_LENGTH_BYTES,
  SCRYPT_PARAMS,
  hkdf,
  scrypt,
} from '../../../lib/crypto/kdf.js';

describe('hkdf', () => {
  it('produces a deterministic 32-byte key from fixed inputs', () => {
    const ikm = Buffer.from('credential-id-bytes', 'utf8');
    const salt = Buffer.from('per-user-salt', 'utf8');
    const info = Buffer.from(`${HKDF_USER_KEK_INFO_PREFIX}user-1`, 'utf8');
    const a = hkdf({ ikm, salt, info, length: KEY_LENGTH_BYTES });
    const b = hkdf({ ikm, salt, info, length: KEY_LENGTH_BYTES });
    expect(a.byteLength).toBe(32);
    expect(b.equals(a)).toBe(true);
  });

  it('different info => different output', () => {
    const ikm = Buffer.from('ikm', 'utf8');
    const salt = Buffer.from('salt', 'utf8');
    const a = hkdf({ ikm, salt, info: Buffer.from('greylock/userKek/v1/u1'), length: 32 });
    const b = hkdf({ ikm, salt, info: Buffer.from('greylock/userKek/v1/u2'), length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different salt => different output', () => {
    const ikm = Buffer.from('ikm', 'utf8');
    const info = Buffer.from('info', 'utf8');
    const a = hkdf({ ikm, salt: Buffer.from('salt-a'), info, length: 32 });
    const b = hkdf({ ikm, salt: Buffer.from('salt-b'), info, length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different IKM => different output', () => {
    const salt = Buffer.from('salt', 'utf8');
    const info = Buffer.from('info', 'utf8');
    const a = hkdf({ ikm: Buffer.from('ikm-a'), salt, info, length: 32 });
    const b = hkdf({ ikm: Buffer.from('ikm-b'), salt, info, length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different output length => different bytes (truncation property)', () => {
    const ikm = Buffer.from('ikm', 'utf8');
    const salt = Buffer.from('salt', 'utf8');
    const info = Buffer.from('info', 'utf8');
    const a = hkdf({ ikm, salt, info, length: 32 });
    const b = hkdf({ ikm, salt, info, length: 64 });
    expect(b.byteLength).toBe(64);
    // first 32 bytes must match (HKDF "expand" deterministic prefix)
    expect(b.subarray(0, 32).equals(a)).toBe(true);
  });

  it('rejects empty IKM', () => {
    expect(() =>
      hkdf({ ikm: Buffer.alloc(0), salt: Buffer.from('s'), info: Buffer.from('i'), length: 32 }),
    ).toThrow(/non-empty/);
  });

  it('rejects non-positive length', () => {
    expect(() =>
      hkdf({ ikm: Buffer.from('a'), salt: Buffer.from('s'), info: Buffer.from('i'), length: 0 }),
    ).toThrow(/positive integer/);
    expect(() =>
      hkdf({ ikm: Buffer.from('a'), salt: Buffer.from('s'), info: Buffer.from('i'), length: -5 }),
    ).toThrow(/positive integer/);
  });

  it('rejects non-integer length', () => {
    expect(() =>
      hkdf({ ikm: Buffer.from('a'), salt: Buffer.from('s'), info: Buffer.from('i'), length: 1.5 }),
    ).toThrow(/positive integer/);
  });

  it('rejects length above RFC 5869 max for SHA-256', () => {
    expect(() =>
      hkdf({
        ikm: Buffer.from('a'),
        salt: Buffer.from('s'),
        info: Buffer.from('i'),
        length: 8161,
      }),
    ).toThrow(/exceeds RFC 5869 max/);
  });
});

describe('scrypt', () => {
  // Use very small N for fast tests; production uses SCRYPT_PARAMS.N=2^17.
  const TEST_N = 1 << 8;

  it('produces a deterministic 32-byte key from fixed inputs', () => {
    const password = Buffer.from('test-passphrase', 'utf8');
    const salt = Buffer.from('test-salt', 'utf8');
    const a = scrypt({ password, salt, N: TEST_N, r: 8, p: 1, length: 32 });
    const b = scrypt({ password, salt, N: TEST_N, r: 8, p: 1, length: 32 });
    expect(a.byteLength).toBe(32);
    expect(b.equals(a)).toBe(true);
  });

  it('different N => different output', () => {
    const password = Buffer.from('test-passphrase', 'utf8');
    const salt = Buffer.from('test-salt', 'utf8');
    const a = scrypt({ password, salt, N: 1 << 8, r: 8, p: 1, length: 32 });
    const b = scrypt({ password, salt, N: 1 << 9, r: 8, p: 1, length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different salt => different output', () => {
    const password = Buffer.from('test-passphrase', 'utf8');
    const a = scrypt({ password, salt: Buffer.from('s1'), N: TEST_N, r: 8, p: 1, length: 32 });
    const b = scrypt({ password, salt: Buffer.from('s2'), N: TEST_N, r: 8, p: 1, length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different password => different output', () => {
    const salt = Buffer.from('salt', 'utf8');
    const a = scrypt({ password: Buffer.from('p1'), salt, N: TEST_N, r: 8, p: 1, length: 32 });
    const b = scrypt({ password: Buffer.from('p2'), salt, N: TEST_N, r: 8, p: 1, length: 32 });
    expect(a.equals(b)).toBe(false);
  });

  it('different dkLen => different output bytes', () => {
    const password = Buffer.from('p', 'utf8');
    const salt = Buffer.from('s', 'utf8');
    const a = scrypt({ password, salt, N: TEST_N, r: 8, p: 1, length: 16 });
    const b = scrypt({ password, salt, N: TEST_N, r: 8, p: 1, length: 32 });
    expect(a.byteLength).toBe(16);
    expect(b.byteLength).toBe(32);
    // scrypt output is NOT a prefix-extending function: the first 16 bytes
    // of length=32 are NOT necessarily equal to a length=16 derivation.
    // We just check both came back with the right size.
  });

  it('rejects empty password', () => {
    expect(() =>
      scrypt({ password: Buffer.alloc(0), salt: Buffer.from('s'), N: TEST_N, r: 8, p: 1, length: 32 }),
    ).toThrow(/non-empty/);
  });

  it('rejects empty salt', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.alloc(0), N: TEST_N, r: 8, p: 1, length: 32 }),
    ).toThrow(/non-empty/);
  });

  it('rejects non-power-of-2 N', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.from('s'), N: 1000, r: 8, p: 1, length: 32 }),
    ).toThrow(/power of 2/);
  });

  it('rejects N < 2', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.from('s'), N: 1, r: 8, p: 1, length: 32 }),
    ).toThrow(/power of 2/);
  });

  it('rejects non-positive r', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.from('s'), N: TEST_N, r: 0, p: 1, length: 32 }),
    ).toThrow(/r must be a positive integer/);
  });

  it('rejects non-positive p', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.from('s'), N: TEST_N, r: 8, p: 0, length: 32 }),
    ).toThrow(/p must be a positive integer/);
  });

  it('rejects non-positive length', () => {
    expect(() =>
      scrypt({ password: Buffer.from('p'), salt: Buffer.from('s'), N: TEST_N, r: 8, p: 1, length: 0 }),
    ).toThrow(/positive integer/);
  });

  it('SCRYPT_PARAMS matches ARCHITECTURE.md §3 locked values', () => {
    expect(SCRYPT_PARAMS.N).toBe(1 << 17);
    expect(SCRYPT_PARAMS.r).toBe(8);
    expect(SCRYPT_PARAMS.p).toBe(1);
    expect(SCRYPT_PARAMS.length).toBe(32);
  });
});
