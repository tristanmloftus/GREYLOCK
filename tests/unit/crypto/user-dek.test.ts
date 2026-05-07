// Tests for `lib/crypto/user-dek.ts`.

import { randomBytes } from 'node:crypto';
import { describe, it, expect } from 'vitest';

import { aadForUserDekWrap } from '../../../lib/crypto/aad.js';
import { isEnvelopeFailure, open } from '../../../lib/crypto/envelope.js';
import { deriveUserKek, unwrapUserDek, withDerivedUserKek, wrapUserDek } from '../../../lib/crypto/user-dek.js';
import { DomainTag, EncryptedBlob, UserId } from '../../../lib/types/domain.js';

const utf8 = (b: Uint8Array): string => Buffer.from(b).toString('utf8');

const userId = UserId('usr_rory');
const credentialId = randomBytes(32);
const kekSalt = randomBytes(16);
const pepperBytes = randomBytes(32);

describe('deriveUserKek', () => {
  it('is deterministic for fixed inputs', () => {
    const a = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const b = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    expect(a.equals(b)).toBe(true);
    expect(a.byteLength).toBe(32);
  });

  it('different userId => different KEK (HKDF info changes)', () => {
    const a = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const b = deriveUserKek({ userId: UserId('usr_other'), credentialId, kekSalt, pepperBytes });
    expect(a.equals(b)).toBe(false);
  });

  it('different credentialId => different KEK', () => {
    const a = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const b = deriveUserKek({ userId, credentialId: randomBytes(32), kekSalt, pepperBytes });
    expect(a.equals(b)).toBe(false);
  });

  it('different kekSalt => different KEK', () => {
    const a = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const b = deriveUserKek({ userId, credentialId, kekSalt: randomBytes(16), pepperBytes });
    expect(a.equals(b)).toBe(false);
  });

  it('different pepper => different KEK', () => {
    const a = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const b = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes: randomBytes(32) });
    expect(a.equals(b)).toBe(false);
  });

  it('rejects empty credentialId', () => {
    expect(() =>
      deriveUserKek({ userId, credentialId: Buffer.alloc(0), kekSalt, pepperBytes }),
    ).toThrow(/credentialId/);
  });

  it('rejects empty kekSalt', () => {
    expect(() =>
      deriveUserKek({ userId, credentialId, kekSalt: Buffer.alloc(0), pepperBytes }),
    ).toThrow(/kekSalt/);
  });
});

describe('wrapUserDek + unwrapUserDek round-trip', () => {
  it('wraps under the expected AAD and unwraps back to the same DEK', () => {
    const dek = randomBytes(32);
    const wrapped = wrapUserDek({ userId, credentialId, kekSalt, pepperBytes, dekMaterial: dek });
    expect(wrapped[0]).toBe(0x01); // version
    expect(wrapped[1]).toBe(DomainTag.Personal);

    // Verify AAD by manually opening with the derived KEK.
    const kek = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const aad = aadForUserDekWrap({ userId });
    const opened = open({ key: kek, aad, blob: wrapped, expectedDomainTag: DomainTag.Personal });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect(Buffer.from(opened as Uint8Array).equals(dek)).toBe(true);

    const ok = unwrapUserDek({ userId, credentialId, kekSalt, pepperBytes, wrappedUserDek: wrapped });
    expect(ok.ok).toBe(true);
    if (ok.ok) {
      expect(ok.dek.equals(dek)).toBe(true);
    }
  });

  it('AAD is utf8("personal:userdek:" + userId)', () => {
    expect(utf8(aadForUserDekWrap({ userId }))).toBe('personal:userdek:usr_rory');
  });

  it('rejects unwrap with wrong userId (AAD mismatch within domain => tag_invalid)', () => {
    const dek = randomBytes(32);
    const wrapped = wrapUserDek({ userId, credentialId, kekSalt, pepperBytes, dekMaterial: dek });
    const res = unwrapUserDek({
      userId: UserId('usr_attacker'),
      credentialId,
      kekSalt,
      pepperBytes,
      wrappedUserDek: wrapped,
    });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      // Different userId => different KEK derivation AND different AAD; the
      // GCM tag check fails => tag_invalid.
      expect(res.kind).toBe('tag_invalid');
    }
  });

  it('rejects unwrap with wrong credentialId', () => {
    const dek = randomBytes(32);
    const wrapped = wrapUserDek({ userId, credentialId, kekSalt, pepperBytes, dekMaterial: dek });
    const res = unwrapUserDek({
      userId,
      credentialId: randomBytes(32),
      kekSalt,
      pepperBytes,
      wrappedUserDek: wrapped,
    });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('tag_invalid');
    }
  });

  it('rejects unwrap of a PCC-tagged blob (cross-domain => aad_mismatch)', () => {
    // Build a "wrap" with PCC domain tag using the same KEK; should be
    // rejected by the personal-side unwrap due to expectedDomainTag check.
    const dek = randomBytes(32);
    const wrapped = wrapUserDek({ userId, credentialId, kekSalt, pepperBytes, dekMaterial: dek });
    const tampered = Buffer.from(wrapped);
    tampered[1] = DomainTag.Pcc;
    const res = unwrapUserDek({
      userId,
      credentialId,
      kekSalt,
      pepperBytes,
      wrappedUserDek: EncryptedBlob.unsafeFromBytes(tampered),
    });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('aad_mismatch');
    }
  });

  it('rejects wrap of non-32-byte DEK material', () => {
    expect(() =>
      wrapUserDek({ userId, credentialId, kekSalt, pepperBytes, dekMaterial: randomBytes(16) }),
    ).toThrow(/dekMaterial/);
  });

  it('rejects unwrap if payload byte length != 32 (malformed_blob)', async () => {
    // Hand-construct a wrap with a 16-byte payload using the actual KEK.
    // Synthesize a wrap with a 16-byte plaintext but the personal-AAD context
    // for our user. The unwrap-side length check should reject it.
    const { seal } = await import('../../../lib/crypto/envelope.js');
    const kek = deriveUserKek({ userId, credentialId, kekSalt, pepperBytes });
    const aad = aadForUserDekWrap({ userId });
    const shortPayload = randomBytes(16);
    const blob = seal({ key: kek, aad, plaintext: shortPayload, domainTag: DomainTag.Personal });
    const res = unwrapUserDek({
      userId,
      credentialId,
      kekSalt,
      pepperBytes,
      wrappedUserDek: blob,
    });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('malformed_blob');
    }
  });
});

describe('withDerivedUserKek', () => {
  it('passes the derived KEK and zeroizes it after use', async () => {
    let captured: Uint8Array | null = null;
    const result = await withDerivedUserKek(
      { userId, credentialId, kekSalt, pepperBytes },
      async (kek) => {
        captured = kek;
        expect(kek.byteLength).toBe(32);
        return kek.byteLength;
      },
    );
    expect(result).toBe(32);
    expect(captured).not.toBeNull();
    // After the helper returns, the buffer should be zeroized.
    const c = captured as unknown as Buffer;
    expect(c.every((x) => x === 0)).toBe(true);
  });

  it('zeroizes even if useFn rejects', async () => {
    let captured: Uint8Array | null = null;
    await expect(
      withDerivedUserKek(
        { userId, credentialId, kekSalt, pepperBytes },
        async (kek) => {
          captured = kek;
          throw new Error('use-failure');
        },
      ),
    ).rejects.toThrow('use-failure');
    expect(captured).not.toBeNull();
    const c = captured as unknown as Buffer;
    expect(c.every((x) => x === 0)).toBe(true);
  });
});
