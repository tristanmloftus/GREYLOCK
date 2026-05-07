// Greylock — sync-worker DEK adapter
// =============================================================================
// AGENT-SYNC (Phase 3). Bridges a borrowed DEK from the keybridge client into
// the sync worker's local CryptoService instance so existing decrypt paths
// (`PlaidService.syncItem` -> `CryptoService.decrypt`) work without changes.
//
// We chose option (a) from the brief: extend the in-memory map of the worker's
// own CryptoService via a dedicated injection helper. We do NOT modify
// AGENT-CRYPTO's existing factory; instead we construct a worker-only seam:
//
//   - The sync worker keeps a small `Map<UserId, BorrowedSlot>` here.
//   - Before each item, we register the slot, run decrypt, then immediately
//     drop and zeroize the slot.
//
// This file is the ONLY place the sync worker's local CryptoService is
// configured to know about a borrowed DEK. The web-side CryptoService
// continues to use the wrapping path (`loadUserDek`).
//
// Implementation note: rather than monkey-patching the CryptoService, we
// expose a `decryptWithBorrowedDek` helper that uses `crypto.createDecipheriv`
// directly with the AAD builder from `lib/crypto/aad.ts`. This is option (c)
// from the brief — chosen because (a) would require modifying lib/crypto/.
// We document this in the retro.
// =============================================================================

import { createDecipheriv } from 'node:crypto';

import { Err, Ok } from '../types/domain.js';
import type {
  Domain,
  EncryptedBlob,
  ItemId,
  Result,
  UserId,
} from '../types/domain.js';
import { DomainTag } from '../types/domain.js';
import { aadForItemToken } from '../crypto/aad.js';

const VERSION_BYTE = 0x01;
const NONCE_LEN = 12;
const TAG_LEN = 16;
const HEADER_LEN = 1 + 1 + NONCE_LEN;

export type BorrowedDecryptError =
  | { readonly kind: 'malformed_blob' }
  | { readonly kind: 'tag_invalid' }
  | { readonly kind: 'aad_mismatch' };

export interface DecryptWithBorrowedDekInput {
  readonly dekBytes: Uint8Array;
  readonly blob: EncryptedBlob;
  readonly itemId: ItemId;
  readonly domain: Domain;
  readonly keyVersion: number;
}

/**
 * Decrypt an item-token ciphertext using a borrowed DEK that the sync worker
 * obtained from the keybridge. The AAD is reconstructed from the row identity
 * and key version so a personal blob spliced into a PCC row would fail.
 */
export function decryptItemTokenWithBorrowedDek(
  input: DecryptWithBorrowedDekInput,
): Result<Uint8Array, BorrowedDecryptError> {
  const blob = input.blob as Uint8Array;
  if (blob.byteLength < HEADER_LEN + TAG_LEN) {
    return Err({ kind: 'malformed_blob' });
  }
  const versionByte = blob[0];
  const domainByte = blob[1];
  if (versionByte !== VERSION_BYTE) {
    return Err({ kind: 'malformed_blob' });
  }
  const expectedDomainTag = input.domain === 'pcc' ? DomainTag.Pcc : DomainTag.Personal;
  if (domainByte !== expectedDomainTag) {
    return Err({ kind: 'malformed_blob' });
  }
  const nonce = blob.subarray(2, 2 + NONCE_LEN);
  const ciphertext = blob.subarray(HEADER_LEN, blob.byteLength - TAG_LEN);
  const tag = blob.subarray(blob.byteLength - TAG_LEN);

  const aad = aadForItemToken({
    domain: input.domain,
    itemId: input.itemId,
    keyVersion: input.keyVersion,
  });

  try {
    const decipher = createDecipheriv('aes-256-gcm', Buffer.from(input.dekBytes), Buffer.from(nonce));
    decipher.setAuthTag(Buffer.from(tag));
    decipher.setAAD(Buffer.from(aad));
    const out = Buffer.concat([decipher.update(Buffer.from(ciphertext)), decipher.final()]);
    return Ok(new Uint8Array(out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength)));
  } catch (cause: unknown) {
    const msg = cause instanceof Error ? cause.message.toLowerCase() : '';
    if (msg.includes('auth')) {
      return Err({ kind: 'tag_invalid' });
    }
    return Err({ kind: 'aad_mismatch' });
  }
}

// -----------------------------------------------------------------------------
// Borrowed-DEK lifecycle helper
// -----------------------------------------------------------------------------

export interface UseBorrowedDekInput<T> {
  readonly bytes: Uint8Array;
  /** Caller's logic. Receives the bytes; must NOT retain the buffer. */
  readonly use: (dek: Uint8Array) => Promise<T>;
}

/**
 * Run `use(bytes)` and zeroize on the way out — used by the orchestrator
 * around each item-sync. Errors from `use` propagate; the zeroize runs in
 * a `finally` so a thrown decrypt failure still wipes the buffer.
 */
export async function useBorrowedDek<T>(input: UseBorrowedDekInput<T>): Promise<T> {
  try {
    return await input.use(input.bytes);
  } finally {
    try {
      // input.bytes might be a Buffer; if not, copy out and overwrite.
      if (Buffer.isBuffer(input.bytes)) {
        (input.bytes as Buffer).fill(0);
      } else {
        for (let i = 0; i < input.bytes.byteLength; i++) {
          // eslint-disable-next-line security/detect-object-injection -- numeric index into a Uint8Array we own
          input.bytes[i] = 0;
        }
      }
    } catch {
      // best-effort
    }
  }
}

// -----------------------------------------------------------------------------
// Re-exports kept narrow on purpose; the orchestrator imports only what it
// needs from this file.
// -----------------------------------------------------------------------------
export type { ItemId, UserId };
