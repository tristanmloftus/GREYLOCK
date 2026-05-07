// Tests for `lib/crypto/envelope.ts`.
//
// Coverage of the locked spec:
//   - blob layout: version(1) || domain_tag(1) || nonce(12) || ct || tag(16)
//   - nonce uniqueness over N=10000
//   - tag tampering => tag_invalid
//   - AAD mismatch in the same domain => tag_invalid (GCM-driven)
//   - cross-domain substitution => aad_mismatch (pre-flight domain-tag check)
//   - tampered domain_tag => aad_mismatch or malformed_blob
//   - tampered version byte => malformed_blob

import { randomBytes } from 'node:crypto';

import { describe, it, expect } from 'vitest';

import {
  BLOB_VERSION,
  HEADER_LEN,
  NONCE_LEN,
  TAG_LEN,
  isEnvelopeFailure,
  open,
  seal,
} from '../../../lib/crypto/envelope.js';
import { DomainTag, EncryptedBlob } from '../../../lib/types/domain.js';

const KEY = (): Buffer => randomBytes(32);

describe('seal/open round-trip', () => {
  it('PCC round-trip', () => {
    const key = KEY();
    const aad = Buffer.from('pcc:itemtoken:item_a:1', 'utf8');
    const pt = Buffer.from('access-sandbox-1234', 'utf8');
    const blob = seal({ key, aad, plaintext: pt, domainTag: DomainTag.Pcc });
    expect(blob[0]).toBe(BLOB_VERSION);
    expect(blob[1]).toBe(DomainTag.Pcc);
    expect(blob.byteLength).toBe(HEADER_LEN + pt.byteLength + TAG_LEN);
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect(Buffer.from(opened as Uint8Array).equals(pt)).toBe(true);
  });

  it('personal round-trip', () => {
    const key = KEY();
    const aad = Buffer.from('personal:itemtoken:item_p:2', 'utf8');
    const pt = Buffer.from('personal-token-abc', 'utf8');
    const blob = seal({ key, aad, plaintext: pt, domainTag: DomainTag.Personal });
    expect(blob[1]).toBe(DomainTag.Personal);
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Personal });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect(Buffer.from(opened as Uint8Array).equals(pt)).toBe(true);
  });

  it('user-dek wrap round-trip', () => {
    const key = KEY();
    const aad = Buffer.from('personal:userdek:usr_x', 'utf8');
    const dek = randomBytes(32);
    const blob = seal({ key, aad, plaintext: dek, domainTag: DomainTag.Personal });
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Personal });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect(Buffer.from(opened as Uint8Array).equals(dek)).toBe(true);
  });

  it('pcc dek wrap round-trip', () => {
    const key = KEY();
    const aad = Buffer.from('pcc:dekwrap:v3', 'utf8');
    const dek = randomBytes(32);
    const blob = seal({ key, aad, plaintext: dek, domainTag: DomainTag.Pcc });
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect(Buffer.from(opened as Uint8Array).equals(dek)).toBe(true);
  });

  it('handles empty plaintext', () => {
    const key = KEY();
    const aad = Buffer.from('aad', 'utf8');
    const blob = seal({ key, aad, plaintext: Buffer.alloc(0), domainTag: DomainTag.Pcc });
    expect(blob.byteLength).toBe(HEADER_LEN + TAG_LEN);
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(false);
    expect((opened as Uint8Array).byteLength).toBe(0);
  });
});

describe('seal: nonce uniqueness', () => {
  it('produces 10000 distinct nonces under the same key', () => {
    const key = KEY();
    const aad = Buffer.from('pcc:itemtoken:item:1', 'utf8');
    const pt = Buffer.from('payload', 'utf8');
    const seen = new Set<string>();
    const N = 10000;
    for (let i = 0; i < N; i++) {
      const blob = seal({ key, aad, plaintext: pt, domainTag: DomainTag.Pcc });
      const nonce = Buffer.from(blob.subarray(2, 2 + NONCE_LEN)).toString('hex');
      seen.add(nonce);
    }
    expect(seen.size).toBe(N);
  });
});

describe('open: failure modes', () => {
  function makeBlob(): { blob: EncryptedBlob; key: Buffer; aad: Buffer; pt: Buffer } {
    const key = KEY();
    const aad = Buffer.from('pcc:itemtoken:item:1', 'utf8');
    const pt = Buffer.from('plaintext-bytes', 'utf8');
    const blob = seal({ key, aad, plaintext: pt, domainTag: DomainTag.Pcc });
    return { blob, key, aad, pt };
  }

  it('tampered ciphertext byte => tag_invalid', () => {
    const { blob, key, aad } = makeBlob();
    const tampered = Buffer.from(blob);
    // flip a bit in the ciphertext region (after header, before tag).
    const idx = HEADER_LEN + 2;
    tampered[idx] = (tampered[idx] ?? 0) ^ 0x01;
    const opened = open({ key, aad, blob: EncryptedBlob.unsafeFromBytes(tampered), expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('tag_invalid');
  });

  it('tampered tag byte => tag_invalid', () => {
    const { blob, key, aad } = makeBlob();
    const tampered = Buffer.from(blob);
    const idx = tampered.byteLength - 1;
    tampered[idx] = (tampered[idx] ?? 0) ^ 0x80;
    const opened = open({ key, aad, blob: EncryptedBlob.unsafeFromBytes(tampered), expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('tag_invalid');
  });

  it('mismatched AAD (in-domain) => tag_invalid', () => {
    const { blob, key } = makeBlob();
    const wrongAad = Buffer.from('pcc:itemtoken:item:2', 'utf8');
    const opened = open({ key, aad: wrongAad, blob, expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('tag_invalid');
  });

  it('cross-domain substitution => aad_mismatch (pre-flight domain-tag rejection)', () => {
    // Encrypt as PCC, then attempt to open as personal with the equivalent
    // AAD construction string. The domain tag byte differs and we reject
    // before calling GCM.
    const { blob, key, aad } = makeBlob();
    const opened = open({ key, aad, blob, expectedDomainTag: DomainTag.Personal });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('aad_mismatch');
  });

  it('tampered domain_tag byte (unknown) => malformed_blob', () => {
    const { blob, key, aad } = makeBlob();
    const tampered = Buffer.from(blob);
    tampered[1] = 0x99; // not 0x01 / 0x02
    const opened = open({ key, aad, blob: EncryptedBlob.unsafeFromBytes(tampered), expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('malformed_blob');
  });

  it('tampered domain_tag byte (other valid) => aad_mismatch', () => {
    // Personal -> Pcc swap of the on-disk byte while caller still expects Pcc.
    const { blob, key, aad } = makeBlob();
    const tampered = Buffer.from(blob);
    tampered[1] = DomainTag.Personal; // attacker tries to relabel a PCC blob as personal
    const opened = open({ key, aad, blob: EncryptedBlob.unsafeFromBytes(tampered), expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('aad_mismatch');
  });

  it('tampered version byte => malformed_blob', () => {
    const { blob, key, aad } = makeBlob();
    const tampered = Buffer.from(blob);
    tampered[0] = 0x02; // unsupported version
    const opened = open({ key, aad, blob: EncryptedBlob.unsafeFromBytes(tampered), expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('malformed_blob');
  });

  it('truncated blob => malformed_blob', () => {
    const tooShort = Buffer.alloc(5);
    const opened = open({
      key: KEY(),
      aad: Buffer.from('a'),
      blob: EncryptedBlob.unsafeFromBytes(tooShort),
      expectedDomainTag: DomainTag.Pcc,
    });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('malformed_blob');
  });

  it('wrong key => tag_invalid', () => {
    const { blob, aad } = makeBlob();
    const opened = open({ key: KEY(), aad, blob, expectedDomainTag: DomainTag.Pcc });
    expect(isEnvelopeFailure(opened)).toBe(true);
    expect((opened as { kind: string }).kind).toBe('tag_invalid');
  });
});

describe('seal: programmer-error guards', () => {
  it('throws if key is not 32 bytes', () => {
    expect(() =>
      seal({
        key: Buffer.alloc(16),
        aad: Buffer.from('a'),
        plaintext: Buffer.from('p'),
        domainTag: DomainTag.Pcc,
      }),
    ).toThrow(/key must be 32 bytes/);
  });

  it('throws if open is given a non-32-byte key', () => {
    const key = KEY();
    const aad = Buffer.from('a');
    const blob = seal({ key, aad, plaintext: Buffer.from('p'), domainTag: DomainTag.Pcc });
    expect(() =>
      open({ key: Buffer.alloc(16), aad, blob, expectedDomainTag: DomainTag.Pcc }),
    ).toThrow(/key must be 32 bytes/);
  });
});
