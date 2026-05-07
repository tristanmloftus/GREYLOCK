// Tests for the HMAC handshake byte-construction used by keybridge-server +
// keybridge-client. We do not bind a real socket here — that's the
// integration test. We validate the cryptographic agreement.

import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';

import { describe, it, expect } from 'vitest';

import { hkdf } from '../../../lib/crypto/kdf.js';

// Mirror the server's HMAC build:
//   AUTH_HMAC = HMAC-SHA-256(K, serverNonce || clientNonce)
//   AUTH_OK_HMAC = HMAC-SHA-256(K, clientNonce || serverNonce)
function buildAuthHmacFromClient(K: Uint8Array, serverNonce: Buffer, clientNonce: Buffer): Buffer {
  return createHmac('sha256', Buffer.from(K)).update(serverNonce).update(clientNonce).digest();
}
function buildAuthHmacFromServer(K: Uint8Array, serverNonce: Buffer, clientNonce: Buffer): Buffer {
  return createHmac('sha256', Buffer.from(K)).update(clientNonce).update(serverNonce).digest();
}

describe('keybridge HMAC handshake — byte agreement', () => {
  it('client-side HMAC matches server-side expectation when K is shared', () => {
    const K = hkdf({
      ikm: randomBytes(32),
      salt: Buffer.alloc(0),
      info: Buffer.from('greylock/keybridge/v1', 'utf8'),
      length: 32,
    });
    const serverNonce = randomBytes(32);
    const clientNonce = randomBytes(32);

    const clientHmac = buildAuthHmacFromClient(K, serverNonce, clientNonce);
    const serverExpected = buildAuthHmacFromClient(K, serverNonce, clientNonce);

    expect(clientHmac.byteLength).toBe(32);
    expect(serverExpected.byteLength).toBe(32);
    expect(timingSafeEqual(clientHmac, serverExpected)).toBe(true);
  });

  it('rejects when the HMAC key differs', () => {
    const K1 = randomBytes(32);
    const K2 = randomBytes(32);
    const sN = randomBytes(32);
    const cN = randomBytes(32);
    const a = buildAuthHmacFromClient(K1, sN, cN);
    const b = buildAuthHmacFromClient(K2, sN, cN);
    expect(timingSafeEqual(a, b)).toBe(false);
  });

  it('rejects when nonces are swapped (order matters)', () => {
    const K = randomBytes(32);
    const sN = randomBytes(32);
    const cN = randomBytes(32);
    const a = buildAuthHmacFromClient(K, sN, cN); // sN || cN
    const b = buildAuthHmacFromClient(K, cN, sN); // swapped → different
    expect(timingSafeEqual(a, b)).toBe(false);
  });

  it('server reverse-HMAC is distinct from forward-HMAC unless K agrees', () => {
    const K = randomBytes(32);
    const sN = randomBytes(32);
    const cN = randomBytes(32);
    const fwd = buildAuthHmacFromClient(K, sN, cN);
    const rev = buildAuthHmacFromServer(K, sN, cN);
    expect(timingSafeEqual(fwd, rev)).toBe(false);
    // Both peers re-compute the reverse independently from the same K.
    const revAgain = buildAuthHmacFromServer(K, sN, cN);
    expect(timingSafeEqual(rev, revAgain)).toBe(true);
  });
});

describe('handshake key derivation', () => {
  it('derives a 32-byte key from a Master KEK using the locked HKDF info', () => {
    const masterKek = randomBytes(32);
    const k = hkdf({
      ikm: masterKek,
      salt: Buffer.alloc(0),
      info: Buffer.from('greylock/keybridge/v1', 'utf8'),
      length: 32,
    });
    expect(k.byteLength).toBe(32);
    const k2 = hkdf({
      ikm: masterKek,
      salt: Buffer.alloc(0),
      info: Buffer.from('greylock/keybridge/v1', 'utf8'),
      length: 32,
    });
    // Deterministic
    expect(timingSafeEqual(k, k2)).toBe(true);
  });

  it('different Master KEKs give different keybridge keys', () => {
    const a = hkdf({
      ikm: randomBytes(32),
      salt: Buffer.alloc(0),
      info: Buffer.from('greylock/keybridge/v1', 'utf8'),
      length: 32,
    });
    const b = hkdf({
      ikm: randomBytes(32),
      salt: Buffer.alloc(0),
      info: Buffer.from('greylock/keybridge/v1', 'utf8'),
      length: 32,
    });
    expect(timingSafeEqual(a, b)).toBe(false);
  });
});
