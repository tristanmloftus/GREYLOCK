// Tests for `lib/crypto/pcc-dek.ts`.

import { randomBytes } from 'node:crypto';
import { describe, it, expect } from 'vitest';

import { aadForItemToken, aadForPccDekWrap } from '../../../lib/crypto/aad.js';
import { isEnvelopeFailure, open, seal } from '../../../lib/crypto/envelope.js';
import {
  rotateMaster,
  unwrapPccDek,
  wrapPccDek,
} from '../../../lib/crypto/pcc-dek.js';
import { DomainTag, EncryptedBlob, ItemId } from '../../../lib/types/domain.js';

describe('wrapPccDek + unwrapPccDek', () => {
  it('round-trip succeeds', () => {
    const masterKek = randomBytes(32);
    const dek = randomBytes(32);
    const wrapped = wrapPccDek({ masterKek, version: 1, dekMaterial: dek });
    expect(wrapped[1]).toBe(DomainTag.Pcc);
    const res = unwrapPccDek({ masterKek, version: 1, wrappedDek: wrapped });
    expect(res.ok).toBe(true);
    if (res.ok) {
      expect(res.dek.equals(dek)).toBe(true);
    }
  });

  it('AAD format is "pcc:dekwrap:v<n>"', () => {
    expect(Buffer.from(aadForPccDekWrap({ version: 1 })).toString('utf8')).toBe('pcc:dekwrap:v1');
  });

  it('wrong version => tag_invalid (AAD differs)', () => {
    const masterKek = randomBytes(32);
    const dek = randomBytes(32);
    const wrapped = wrapPccDek({ masterKek, version: 1, dekMaterial: dek });
    const res = unwrapPccDek({ masterKek, version: 2, wrappedDek: wrapped });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('tag_invalid');
    }
  });

  it('rejects wrap of non-32-byte DEK material', () => {
    const masterKek = randomBytes(32);
    expect(() => wrapPccDek({ masterKek, version: 1, dekMaterial: randomBytes(16) })).toThrow(
      /dekMaterial/,
    );
  });

  it('rejects unwrap of payload that is not 32 bytes => malformed_blob', () => {
    const masterKek = randomBytes(32);
    const aad = aadForPccDekWrap({ version: 1 });
    // Hand-build a 16-byte payload sealed with the right key + AAD but wrong length.
    const blob = seal({ key: masterKek, aad, plaintext: randomBytes(16), domainTag: DomainTag.Pcc });
    const res = unwrapPccDek({ masterKek, version: 1, wrappedDek: blob });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('malformed_blob');
    }
  });

  it('cross-domain wrap (personal tag) rejected by unwrap with aad_mismatch', () => {
    const masterKek = randomBytes(32);
    const dek = randomBytes(32);
    const aad = aadForPccDekWrap({ version: 1 });
    // Build a personal-tagged blob carrying the PCC AAD; unwrap must reject.
    const personalTagged = seal({ key: masterKek, aad, plaintext: dek, domainTag: DomainTag.Personal });
    const res = unwrapPccDek({ masterKek, version: 1, wrappedDek: personalTagged });
    expect(res.ok).toBe(false);
    if (!res.ok) {
      expect(res.kind).toBe('aad_mismatch');
    }
  });
});

// -----------------------------------------------------------------------------
// rotateMaster
// -----------------------------------------------------------------------------

describe('rotateMaster', () => {
  function freshScenario(itemCount: number) {
    const oldMasterKek = randomBytes(32);
    const oldPccDek = randomBytes(32);
    const oldVersion = 3;
    const newMasterKek = randomBytes(32);
    const newPccDekMaterial = randomBytes(32);
    const newVersion = 4;

    // Pre-seed N PCC item tokens encrypted under the OLD PCC DEK.
    const items: { itemId: ItemId; plaintext: Buffer; blob: EncryptedBlob }[] = [];
    for (let i = 0; i < itemCount; i++) {
      const itemId = ItemId(`itm_${i}`);
      const pt = Buffer.from(`access-token-${i}`, 'utf8');
      const aad = aadForItemToken({ domain: 'pcc', itemId, keyVersion: oldVersion });
      const blob = seal({ key: oldPccDek, aad, plaintext: pt, domainTag: DomainTag.Pcc });
      items.push({ itemId, plaintext: pt, blob });
    }

    return {
      oldMasterKek,
      oldPccDek,
      oldVersion,
      newMasterKek,
      newPccDekMaterial,
      newVersion,
      items,
    };
  }

  it('re-encrypts every PCC item token under the new key + version', async () => {
    const N = 5;
    const s = freshScenario(N);
    const audit: Array<{ kind: string }> = [];

    interface Persisted {
      readonly newVersion: number;
      readonly newWrappedPccDek: EncryptedBlob;
      readonly rewrittenItems: ReadonlyArray<{ itemId: ItemId; blob: EncryptedBlob }>;
    }
    const persistedSlot: { value: Persisted | null } = { value: null };

    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: s.oldPccDek,
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: s.newPccDekMaterial,
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => s.items.map((i) => ({ itemId: i.itemId, blob: i.blob })),
        persistRotation: async (input) => {
          persistedSlot.value = {
            newVersion: input.newVersion,
            newWrappedPccDek: input.newWrappedPccDek,
            rewrittenItems: input.rewrittenItems,
          };
        },
        emitAudit: (e) => audit.push(e),
      },
    });

    expect(result.ok).toBe(true);
    const p = persistedSlot.value;
    if (p === null) {
      throw new Error('persistRotation was never called');
    }
    expect(p.newVersion).toBe(s.newVersion);
    expect(p.rewrittenItems.length).toBe(N);

    // Sanity: the new wrap must be unwrapable with the new master kek.
    const unwrapped = unwrapPccDek({
      masterKek: s.newMasterKek,
      version: s.newVersion,
      wrappedDek: p.newWrappedPccDek,
    });
    expect(unwrapped.ok).toBe(true);

    // Each rewritten item must decrypt under (new DEK, new version AAD)
    // back to the same plaintext.
    for (let i = 0; i < N; i++) {
      const rewritten = p.rewrittenItems[i];
      if (rewritten === undefined) {
        throw new Error('rewritten missing');
      }
      const newAad = aadForItemToken({
        domain: 'pcc',
        itemId: rewritten.itemId,
        keyVersion: s.newVersion,
      });
      const opened = open({
        key: s.newPccDekMaterial,
        aad: newAad,
        blob: rewritten.blob,
        expectedDomainTag: DomainTag.Pcc,
      });
      expect(isEnvelopeFailure(opened)).toBe(false);
      const ptOriginal = s.items[i];
      if (ptOriginal === undefined) {
        throw new Error('ptOriginal missing');
      }
      expect(Buffer.from(opened as Uint8Array).equals(ptOriginal.plaintext)).toBe(true);
    }

    // Audit log: N item_rewritten + 1 wrap_replaced.
    const rewriteEvents = audit.filter((e) => e.kind === 'item_rewritten');
    const wrapEvents = audit.filter((e) => e.kind === 'wrap_replaced');
    expect(rewriteEvents.length).toBe(N);
    expect(wrapEvents.length).toBe(1);
  });

  it('handles zero items', async () => {
    const s = freshScenario(0);
    const audit: Array<{ kind: string }> = [];
    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: s.oldPccDek,
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: s.newPccDekMaterial,
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => [],
        persistRotation: async () => undefined,
        emitAudit: (e) => audit.push(e),
      },
    });
    expect(result.ok).toBe(true);
    expect(audit.length).toBe(1);
    expect(audit[0]?.kind).toBe('wrap_replaced');
  });

  it('returns persist_failed if persistRotation throws (and old wrap stays in use)', async () => {
    const s = freshScenario(1);
    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: s.oldPccDek,
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: s.newPccDekMaterial,
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => s.items.map((i) => ({ itemId: i.itemId, blob: i.blob })),
        persistRotation: async () => {
          throw new Error('db rejected');
        },
        emitAudit: () => undefined,
      },
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('persist_failed');
      // Cause string MUST NOT echo the underlying error message.
      if (result.error.kind === 'persist_failed') {
        expect(result.error.cause).not.toMatch(/db rejected/);
      }
    }
  });

  it('returns malformed_blob if newPccDekMaterial is wrong length', async () => {
    const s = freshScenario(0);
    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: s.oldPccDek,
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: randomBytes(16),
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => [],
        persistRotation: async () => undefined,
        emitAudit: () => undefined,
      },
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('malformed_blob');
    }
  });

  it('returns malformed_blob if oldPccDek is wrong length', async () => {
    const s = freshScenario(0);
    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: randomBytes(16),
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: s.newPccDekMaterial,
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => [],
        persistRotation: async () => undefined,
        emitAudit: () => undefined,
      },
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('malformed_blob');
    }
  });

  it('surfaces tag_invalid with the offending itemId if a row was tampered', async () => {
    const s = freshScenario(2);
    // Tamper the second row.
    const first = s.items[0];
    const second = s.items[1];
    if (first === undefined || second === undefined) {
      throw new Error('seed missing');
    }
    const tampered = Buffer.from(second.blob);
    const lastIdx = tampered.byteLength - 1;
    const lastByte = tampered[lastIdx] ?? 0;
    tampered[lastIdx] = lastByte ^ 0x01;
    const tamperedBlob = EncryptedBlob.unsafeFromBytes(tampered);

    const result = await rotateMaster({
      oldMasterKek: s.oldMasterKek,
      oldPccDek: s.oldPccDek,
      oldVersion: s.oldVersion,
      newMasterKek: s.newMasterKek,
      newPccDekMaterial: s.newPccDekMaterial,
      newVersion: s.newVersion,
      callbacks: {
        readAllPccItemTokens: async () => [
          { itemId: first.itemId, blob: first.blob },
          { itemId: second.itemId, blob: tamperedBlob },
        ],
        persistRotation: async () => undefined,
        emitAudit: () => undefined,
      },
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('tag_invalid');
      if (result.error.kind === 'tag_invalid') {
        expect(result.error.atItemId).toBe(second.itemId);
      }
    }
  });
});
