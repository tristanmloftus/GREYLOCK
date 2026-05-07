// Greylock — per-user DEK derivation + wrap/unwrap
// =============================================================================
// One file per key tier. This is the per-user tier:
//
//   per-user KEK = HKDF-SHA-256(
//                    IKM = credentialId || CRYPTO_PEPPER_BYTES,
//                    salt = Passkey.kekSalt,
//                    info = utf8('greylock/userKek/v1/' + userId),
//                    L = 32)
//
//   per-user DEK = AES-256-GCM.open(KEK, wrappedUserDek,
//                                   aad = utf8('personal:userdek:' + userId))
//
// All buffers are zeroized after use. The KEK never leaves this function;
// only the DEK escapes (and the caller — `index.ts` — holds it in
// module-private state).
// =============================================================================

import type { UserId, EncryptedBlob } from '../types/domain.js';
import { DomainTag } from '../types/domain.js';

import { aadForUserDekWrap } from './aad.js';
import { open, seal, isEnvelopeFailure } from './envelope.js';
import { hkdf, HKDF_USER_KEK_INFO_PREFIX, KEY_LENGTH_BYTES } from './kdf.js';
import { withZeroized, zeroize } from './zeroize.js';

/**
 * Derive the per-user KEK. Returns a fresh 32-byte Buffer. Caller must
 * zeroize. The function builds + zeroizes its own intermediate IKM buffer.
 *
 * Throws on parameter violations (caught at boundary).
 */
export function deriveUserKek(input: {
  readonly userId: UserId;
  readonly credentialId: Uint8Array;
  readonly kekSalt: Uint8Array;
  readonly pepperBytes: Uint8Array;
}): Buffer {
  if (input.credentialId.byteLength === 0) {
    throw new Error('user-dek: credentialId must be non-empty');
  }
  if (input.kekSalt.byteLength === 0) {
    throw new Error('user-dek: kekSalt must be non-empty');
  }

  const ikm = Buffer.concat([
    Buffer.from(input.credentialId),
    Buffer.from(input.pepperBytes),
  ]);
  try {
    const info = Buffer.from(`${HKDF_USER_KEK_INFO_PREFIX}${input.userId}`, 'utf8');
    return hkdf({ ikm, salt: Buffer.from(input.kekSalt), info, length: KEY_LENGTH_BYTES });
  } finally {
    zeroize(ikm);
  }
}

/**
 * Wrap a fresh 32-byte DEK under the per-user KEK. Used during enrollment
 * (CryptoService.wrapUserDek) and rotation. The caller supplies the DEK
 * material (typically `randomBytes(32)`); we wrap it and return the blob.
 */
export function wrapUserDek(input: {
  readonly userId: UserId;
  readonly credentialId: Uint8Array;
  readonly kekSalt: Uint8Array;
  readonly pepperBytes: Uint8Array;
  readonly dekMaterial: Uint8Array;
}): EncryptedBlob {
  if (input.dekMaterial.byteLength !== KEY_LENGTH_BYTES) {
    throw new Error(`user-dek: dekMaterial must be ${KEY_LENGTH_BYTES} bytes`);
  }
  const aad = aadForUserDekWrap({ userId: input.userId });
  const kek = deriveUserKek({
    userId: input.userId,
    credentialId: input.credentialId,
    kekSalt: input.kekSalt,
    pepperBytes: input.pepperBytes,
  });
  try {
    return seal({
      key: kek,
      aad,
      plaintext: input.dekMaterial,
      domainTag: DomainTag.Personal,
    });
  } finally {
    zeroize(kek);
  }
}

/**
 * Unwrap a per-user DEK blob. Returns either:
 *   - { ok: true, dek }  — a fresh 32-byte Buffer the caller now owns.
 *   - { ok: false, ... } — `aad_mismatch` | `tag_invalid` | `malformed_blob`.
 *
 * The caller (`index.ts`) MUST zeroize the returned `dek` Buffer when the
 * user logs out / the session ends.
 */
export function unwrapUserDek(input: {
  readonly userId: UserId;
  readonly credentialId: Uint8Array;
  readonly kekSalt: Uint8Array;
  readonly pepperBytes: Uint8Array;
  readonly wrappedUserDek: EncryptedBlob;
}): { readonly ok: true; readonly dek: Buffer } | { readonly ok: false; readonly kind: 'aad_mismatch' | 'tag_invalid' | 'malformed_blob' } {
  const aad = aadForUserDekWrap({ userId: input.userId });
  const kek = deriveUserKek({
    userId: input.userId,
    credentialId: input.credentialId,
    kekSalt: input.kekSalt,
    pepperBytes: input.pepperBytes,
  });
  try {
    const opened = open({
      key: kek,
      aad,
      blob: input.wrappedUserDek,
      expectedDomainTag: DomainTag.Personal,
    });
    if (isEnvelopeFailure(opened)) {
      return { ok: false, kind: opened.kind };
    }
    if (opened.byteLength !== KEY_LENGTH_BYTES) {
      // The wrap was tampered to encode a non-32-byte payload. Reject.
      // We zeroize and treat it as a malformed_blob — the upper boundary
      // surfaces this as `aad_mismatch` is wrong, `tag_invalid` is wrong,
      // so we use `malformed_blob` to indicate a structural issue.
      const buf = Buffer.from(opened);
      zeroize(buf);
      return { ok: false, kind: 'malformed_blob' };
    }
    return { ok: true, dek: Buffer.from(opened) };
  } finally {
    zeroize(kek);
  }
}

/** Use `withZeroized` ergonomically across this module. */
export function withDerivedUserKek<T>(
  input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly pepperBytes: Uint8Array;
  },
  use: (kek: Buffer) => Promise<T>,
): Promise<T> {
  return withZeroized(() => deriveUserKek(input), (k) => use(k as Buffer));
}
