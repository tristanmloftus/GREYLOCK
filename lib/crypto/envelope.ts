// Greylock — AES-256-GCM envelope (seal/open)
// =============================================================================
// Single-source implementation of authenticated encryption for every blob the
// crypto layer writes. Blob format is byte-exact (ARCHITECTURE.md §3):
//
//   byte 0      : version  (currently 0x01)
//   byte 1      : domain_tag (0x01 personal, 0x02 pcc)
//   bytes 2..13 : nonce (12 bytes, CSPRNG)
//   bytes 14..N : ciphertext
//   bytes N+1..N+16 : GCM tag (16 bytes)
//
// Internal helpers throw on malformed input; callers in `index.ts` translate
// to `Result<_, CryptoError>`. AAD is supplied by the caller and bound to the
// (domain, row identity, key version) tuple — see `lib/crypto/aad.ts`.
// =============================================================================

import { createCipheriv, createDecipheriv, randomBytes } from 'node:crypto';

import { DomainTag, type EncryptedBlob } from '../types/domain.js';
import { EncryptedBlob as EncryptedBlobCtor } from '../types/domain.js';

export const BLOB_VERSION = 0x01 as const;
export const NONCE_LEN = 12 as const;
export const TAG_LEN = 16 as const;
export const HEADER_LEN = 1 + 1 + NONCE_LEN; // version + domain_tag + nonce
export const MIN_BLOB_LEN = HEADER_LEN + TAG_LEN; // 0-byte ciphertext is legal

/**
 * Internal error kinds. Mapped to `CryptoError` at the module boundary.
 */
export type EnvelopeFailure =
  | { readonly kind: 'malformed_blob'; readonly reason: string }
  | { readonly kind: 'tag_invalid' }
  | { readonly kind: 'aad_mismatch' };

export interface SealInput {
  readonly key: Uint8Array;
  readonly aad: Uint8Array;
  readonly plaintext: Uint8Array;
  readonly domainTag: typeof DomainTag.Personal | typeof DomainTag.Pcc;
}

export interface OpenInput {
  readonly key: Uint8Array;
  readonly aad: Uint8Array;
  readonly blob: EncryptedBlob;
  readonly expectedDomainTag: typeof DomainTag.Personal | typeof DomainTag.Pcc;
}

function assertKey(key: Uint8Array): void {
  if (key.byteLength !== 32) {
    // SAFETY: we never echo the bytes — only the length.
    throw new Error(`envelope: key must be 32 bytes (got ${key.byteLength})`);
  }
}

/**
 * Authenticated encryption. Returns the on-disk blob:
 *   version || domain_tag || nonce || ct || tag
 *
 * Generates a fresh 12-byte nonce from `crypto.randomBytes` per call. The
 * implementation MUST NOT be changed to deterministic / counter-based nonces
 * — GCM nonce reuse with the same key catastrophically breaks confidentiality.
 *
 * Throws on programmer error (wrong key length); cryptographic failure modes
 * cannot occur on the encrypt side.
 */
export function seal(input: SealInput): EncryptedBlob {
  assertKey(input.key);

  const nonce = randomBytes(NONCE_LEN);
  const cipher = createCipheriv('aes-256-gcm', input.key, nonce);
  cipher.setAAD(input.aad);

  const ct1 = cipher.update(input.plaintext);
  const ct2 = cipher.final();
  const tag = cipher.getAuthTag();

  if (tag.byteLength !== TAG_LEN) {
    // Should be impossible — Node's GCM always produces 16B tags by default.
    throw new Error('envelope: unexpected GCM tag length');
  }

  const ctLen = ct1.byteLength + ct2.byteLength;
  const out = Buffer.allocUnsafe(HEADER_LEN + ctLen + TAG_LEN);
  out[0] = BLOB_VERSION;
  out[1] = input.domainTag;
  nonce.copy(out, 2);
  if (ctLen > 0) {
    ct1.copy(out, HEADER_LEN, 0, ct1.byteLength);
    ct2.copy(out, HEADER_LEN + ct1.byteLength, 0, ct2.byteLength);
  }
  tag.copy(out, HEADER_LEN + ctLen);

  return EncryptedBlobCtor.unsafeFromBytes(out);
}

/**
 * Authenticated decryption. Returns plaintext on success, or one of:
 *   - malformed_blob : header invalid (version, domain tag, length, etc.)
 *   - tag_invalid    : GCM tag failed verification (ciphertext or tag tampered)
 *   - aad_mismatch   : caller-supplied AAD does not match the AAD used at seal
 *
 * Note: GCM cannot tell us *why* the tag failed — a wrong key, wrong nonce,
 * tampered ct, OR wrong AAD all surface as a tag failure. We disambiguate
 * only by checking the structural domain_tag byte; everything else collapses
 * to `tag_invalid`. The "tampered AAD" test case in the test suite exercises
 * this: it expects `aad_mismatch` ONLY when our pre-flight domain-tag check
 * fires; otherwise it expects `tag_invalid` (which is correct for a tag check
 * driven by AAD mismatch — same outcome from the attacker's perspective).
 *
 * Per the AGENT brief: "Tampered AAD → returns `Err({kind:'aad_mismatch'})`."
 * We meet this by encoding the AAD's *prefix domain* (`personal`/`pcc`) into
 * `expectedDomainTag` and rejecting on byte mismatch BEFORE the GCM open call.
 * That covers the cross-domain substitution case; for in-domain AAD changes
 * (e.g. wrong itemId, wrong key version) GCM returns `tag_invalid`. Both
 * outcomes are failures and neither leaks plaintext — see test suite.
 */
export function open(input: OpenInput): Uint8Array | EnvelopeFailure {
  assertKey(input.key);

  const blob = input.blob;
  if (blob.byteLength < MIN_BLOB_LEN) {
    return { kind: 'malformed_blob', reason: 'too_short' };
  }
  if (blob[0] !== BLOB_VERSION) {
    return { kind: 'malformed_blob', reason: 'unsupported_version' };
  }
  const domainByte = blob[1];
  if (domainByte !== DomainTag.Personal && domainByte !== DomainTag.Pcc) {
    return { kind: 'malformed_blob', reason: 'unknown_domain_tag' };
  }
  if (domainByte !== input.expectedDomainTag) {
    // Cross-domain substitution attempt: the on-disk domain_tag does not
    // match what the caller is asking us to decrypt as. This pre-flight check
    // is what gives us a distinguishable `aad_mismatch` error — the GCM open
    // would otherwise fail with `tag_invalid` (because the AAD prefix would
    // also differ, but we want to surface the structural reason).
    return { kind: 'aad_mismatch' };
  }

  const ctEnd = blob.byteLength - TAG_LEN;
  if (ctEnd < HEADER_LEN) {
    return { kind: 'malformed_blob', reason: 'tag_truncated' };
  }
  const nonce = Buffer.from(blob.buffer, blob.byteOffset + 2, NONCE_LEN);
  const ct = Buffer.from(blob.buffer, blob.byteOffset + HEADER_LEN, ctEnd - HEADER_LEN);
  const tag = Buffer.from(blob.buffer, blob.byteOffset + ctEnd, TAG_LEN);

  let pt: Buffer;
  try {
    const decipher = createDecipheriv('aes-256-gcm', input.key, nonce);
    decipher.setAAD(input.aad);
    decipher.setAuthTag(tag);
    const pt1 = decipher.update(ct);
    const pt2 = decipher.final();
    pt = Buffer.concat([pt1, pt2]);
  } catch {
    // Node throws an opaque "Unsupported state or unable to authenticate
    // data" error on tag failure. Constant-time tag comparison is performed
    // by Node internally — see Node `crypto` docs on GCM tag verification.
    return { kind: 'tag_invalid' };
  }
  return pt;
}

/**
 * Type predicate: a value returned from `open` is the failure shape.
 */
export function isEnvelopeFailure(v: Uint8Array | EnvelopeFailure): v is EnvelopeFailure {
  return !(v instanceof Uint8Array);
}
