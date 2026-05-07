// Greylock — PCC DEK wrap/unwrap + master rotation
// =============================================================================
// PCC tier:
//   PCC DEK = AES-256-GCM.open(MasterKEK, PccKeyWrap.wrappedDek,
//                              aad = utf8('pcc:dekwrap:v' + version))
//
// And the symmetric `wrapPccDek` for newly-generated DEK material (used during
// initial bootstrap and during master rotation).
//
// `rotateMaster` is a callback-driven, repo-agnostic rewrite loop:
//   1. Caller provides the OLD wrap row (loaded by them) + ability to read
//      every `pcc:itemtoken` ciphertext under the OLD master.
//   2. We derive new MasterKEK (via `master-key.ts`) and wrap a NEW PccKeyWrap
//      blob; caller persists it in a transaction.
//   3. For every PCC item token, we decrypt under OLD PCC DEK, re-encrypt
//      under NEW PCC DEK, and the caller persists the rewrite.
//
// All of this is pure crypto: we never touch the DB, the file system, or the
// Keychain. Repos and Keychain access are injected by the caller.
// =============================================================================

import type { ItemId, EncryptedBlob } from '../types/domain.js';
import { DomainTag } from '../types/domain.js';

import { aadForItemToken, aadForPccDekWrap } from './aad.js';
import { open, seal, isEnvelopeFailure } from './envelope.js';
import { KEY_LENGTH_BYTES } from './kdf.js';
import { zeroize } from './zeroize.js';

/**
 * Wrap raw 32-byte PCC DEK material under a Master KEK.
 */
export function wrapPccDek(input: {
  readonly masterKek: Uint8Array;
  readonly version: number;
  readonly dekMaterial: Uint8Array;
}): EncryptedBlob {
  if (input.dekMaterial.byteLength !== KEY_LENGTH_BYTES) {
    throw new Error(`pcc-dek: dekMaterial must be ${KEY_LENGTH_BYTES} bytes`);
  }
  const aad = aadForPccDekWrap({ version: input.version });
  return seal({
    key: input.masterKek,
    aad,
    plaintext: input.dekMaterial,
    domainTag: DomainTag.Pcc,
  });
}

/**
 * Unwrap a PCC DEK blob using the in-memory Master KEK.
 *
 * Returns `{ ok:true, dek }` (a fresh 32-byte Buffer the caller owns) or
 * a structured failure. The caller must zeroize `dek` on shutdown.
 */
export function unwrapPccDek(input: {
  readonly masterKek: Uint8Array;
  readonly version: number;
  readonly wrappedDek: EncryptedBlob;
}): { readonly ok: true; readonly dek: Buffer } | { readonly ok: false; readonly kind: 'aad_mismatch' | 'tag_invalid' | 'malformed_blob' } {
  const aad = aadForPccDekWrap({ version: input.version });
  const opened = open({
    key: input.masterKek,
    aad,
    blob: input.wrappedDek,
    expectedDomainTag: DomainTag.Pcc,
  });
  if (isEnvelopeFailure(opened)) {
    return { ok: false, kind: opened.kind };
  }
  if (opened.byteLength !== KEY_LENGTH_BYTES) {
    const buf = Buffer.from(opened);
    zeroize(buf);
    return { ok: false, kind: 'malformed_blob' };
  }
  return { ok: true, dek: Buffer.from(opened) };
}

// -----------------------------------------------------------------------------
// rotateMaster — pure-crypto rewrite loop
// -----------------------------------------------------------------------------

export interface PccItemTokenRow {
  readonly itemId: ItemId;
  readonly blob: EncryptedBlob;
}

export interface RotateMasterCallbacks {
  /**
   * Read every active PCC item-token row that was encrypted under the OLD
   * `masterKekVersion`. Implementation: `ItemRepository.list({admin}, {domain:pcc})`
   * + `readEncryptedToken` for each. Soft-deleted rows MAY be skipped per the
   * caller's retention policy.
   *
   * Implementations should yield rows lazily (Async iterator) but for v0.1
   * we pass an array; we don't expect more than a few hundred PCC items.
   */
  readonly readAllPccItemTokens: () => Promise<ReadonlyArray<PccItemTokenRow>>;

  /**
   * Persist the new wrap row + commit the rewritten item tokens atomically.
   * Receives the new wrap blob (under the NEW master KEK) and the rewritten
   * item-token rows (each under the NEW PCC DEK with NEW masterKekVersion AAD).
   *
   * Caller is expected to wrap this in a single Prisma `$transaction`.
   * If it throws / rejects, rotation is aborted and the OLD wrap stays in use.
   */
  readonly persistRotation: (input: {
    readonly newVersion: number;
    readonly newWrappedPccDek: EncryptedBlob;
    readonly rewrittenItems: ReadonlyArray<PccItemTokenRow>;
  }) => Promise<void>;

  /**
   * Audit hook. Called once per item rewritten and once for the wrap
   * persistence. Implementations append `AuditAction.AdminMasterRotationStarted`
   * /Completed/Failed and per-item `plaid_token_decrypted` entries; the
   * caller's responsibility — we just emit the events.
   */
  readonly emitAudit: (event:
    | { readonly kind: 'item_rewritten'; readonly itemId: ItemId; readonly oldVersion: number; readonly newVersion: number }
    | { readonly kind: 'wrap_replaced'; readonly oldVersion: number; readonly newVersion: number }
  ) => void;
}

export type RotateMasterFailure =
  | { readonly kind: 'tag_invalid'; readonly atItemId: ItemId | null }
  | { readonly kind: 'aad_mismatch'; readonly atItemId: ItemId | null }
  | { readonly kind: 'malformed_blob'; readonly atItemId: ItemId | null }
  | { readonly kind: 'persist_failed'; readonly cause: string };

export interface RotateMasterInput {
  readonly oldMasterKek: Uint8Array;
  readonly oldPccDek: Uint8Array;
  readonly oldVersion: number;
  readonly newMasterKek: Uint8Array;
  readonly newPccDekMaterial: Uint8Array;
  readonly newVersion: number;
  readonly callbacks: RotateMasterCallbacks;
}

/**
 * Pure rewrite loop. Returns `{ ok:true, oldVersion, newVersion }` on success.
 * On failure, the old wrap remains in use and the caller's persistence layer
 * is never asked to commit anything.
 *
 * Steps:
 *   1. Read every existing PCC item-token under OLD PCC DEK + OLD AAD.
 *   2. Re-encrypt each under NEW PCC DEK + NEW AAD (which references newVersion).
 *   3. Wrap NEW PCC DEK under NEW MasterKEK with AAD `pcc:dekwrap:v<newVersion>`.
 *   4. Hand the bundle to `persistRotation` for atomic persistence.
 */
export async function rotateMaster(input: RotateMasterInput): Promise<
  | { readonly ok: true; readonly oldVersion: number; readonly newVersion: number }
  | { readonly ok: false; readonly error: RotateMasterFailure }
> {
  if (input.newPccDekMaterial.byteLength !== KEY_LENGTH_BYTES) {
    return { ok: false, error: { kind: 'malformed_blob', atItemId: null } };
  }
  if (input.oldPccDek.byteLength !== KEY_LENGTH_BYTES) {
    return { ok: false, error: { kind: 'malformed_blob', atItemId: null } };
  }

  const rows = await input.callbacks.readAllPccItemTokens();
  const rewritten: PccItemTokenRow[] = [];
  for (const row of rows) {
    // Decrypt under OLD PCC DEK with OLD-version AAD.
    const oldAad = aadForItemToken({ domain: 'pcc', itemId: row.itemId, keyVersion: input.oldVersion });
    const opened = open({
      key: input.oldPccDek,
      aad: oldAad,
      blob: row.blob,
      expectedDomainTag: DomainTag.Pcc,
    });
    if (isEnvelopeFailure(opened)) {
      return { ok: false, error: { kind: opened.kind, atItemId: row.itemId } };
    }
    const ptBuf = Buffer.from(opened);
    try {
      // Re-encrypt under NEW PCC DEK with NEW-version AAD.
      const newAad = aadForItemToken({ domain: 'pcc', itemId: row.itemId, keyVersion: input.newVersion });
      const newBlob = seal({
        key: input.newPccDekMaterial,
        aad: newAad,
        plaintext: ptBuf,
        domainTag: DomainTag.Pcc,
      });
      rewritten.push({ itemId: row.itemId, blob: newBlob });
      input.callbacks.emitAudit({
        kind: 'item_rewritten',
        itemId: row.itemId,
        oldVersion: input.oldVersion,
        newVersion: input.newVersion,
      });
    } finally {
      zeroize(ptBuf);
    }
  }

  // Wrap NEW PCC DEK under NEW MasterKEK.
  const newWrap = wrapPccDek({
    masterKek: input.newMasterKek,
    version: input.newVersion,
    dekMaterial: input.newPccDekMaterial,
  });

  try {
    await input.callbacks.persistRotation({
      newVersion: input.newVersion,
      newWrappedPccDek: newWrap,
      rewrittenItems: rewritten,
    });
  } catch (e: unknown) {
    // Sanitize: never echo the underlying error message — it could carry
    // SQL fragments that include item ids, etc. We only surface a fixed string.
    void e;
    return { ok: false, error: { kind: 'persist_failed', cause: 'persistence layer rejected the rotation' } };
  }

  input.callbacks.emitAudit({
    kind: 'wrap_replaced',
    oldVersion: input.oldVersion,
    newVersion: input.newVersion,
  });

  return { ok: true, oldVersion: input.oldVersion, newVersion: input.newVersion };
}
