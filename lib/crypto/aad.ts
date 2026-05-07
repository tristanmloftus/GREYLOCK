// Greylock — AAD construction
// =============================================================================
// AES-256-GCM Additional Authenticated Data is the cryptographic enforcement
// of the domain partition: a personal ciphertext copied into a PCC row will
// fail the GCM tag check on `open` because the AAD computed from the new
// context (`pcc:itemtoken:...`) differs from the AAD bound at encrypt time.
//
// AAD format is LOCKED in ARCHITECTURE.md §3:
//
//   personal item token : utf8("personal:itemtoken:" + itemId + ":" + userDekVersion)
//   pcc item token      : utf8("pcc:itemtoken:" + itemId + ":" + masterKekVersion)
//   per-user DEK wrap   : utf8("personal:userdek:" + userId)
//   pcc DEK wrap        : utf8("pcc:dekwrap:v" + version)
//
// Bytes only. UTF-8. No JSON, no separators that depend on host encoding.
// =============================================================================

import type { ItemId, UserId } from '../types/domain.js';

const utf8 = (s: string): Uint8Array => Buffer.from(s, 'utf8');

function assertSafeKeyVersion(v: number, label: string): void {
  // Versions are positive integers in the range we control. Reject anything
  // that would change the AAD string in a host-dependent way.
  if (!Number.isInteger(v) || v < 1) {
    throw new Error(`aad: ${label} must be a positive integer (got ${String(v)})`);
  }
}

function assertNonEmpty(s: string, label: string): void {
  if (s.length === 0) {
    throw new Error(`aad: ${label} must be non-empty`);
  }
  // Disallow ':' because we use ':' as our delimiter. If an injected id ever
  // contained a colon, an attacker who could pick the id might collide AADs
  // from different tiers. cuid() never contains colons; this is belt-and-braces.
  if (s.includes(':')) {
    throw new Error(`aad: ${label} must not contain ':'`);
  }
}

/**
 * AAD for a personal/pcc Plaid item access-token ciphertext.
 *
 *   personal:itemtoken:<itemId>:<keyVersion>
 *   pcc:itemtoken:<itemId>:<keyVersion>
 *
 * `keyVersion` is `userDekVersion` for personal, `masterKekVersion` for pcc.
 */
export function aadForItemToken(input: {
  readonly domain: 'personal' | 'pcc';
  readonly itemId: ItemId;
  readonly keyVersion: number;
}): Uint8Array {
  assertNonEmpty(input.itemId, 'itemId');
  assertSafeKeyVersion(input.keyVersion, 'keyVersion');
  return utf8(`${input.domain}:itemtoken:${input.itemId}:${input.keyVersion}`);
}

/**
 * AAD for the wrapped per-user DEK blob persisted as `User.wrappedUserDek`.
 *
 *   personal:userdek:<userId>
 */
export function aadForUserDekWrap(input: { readonly userId: UserId }): Uint8Array {
  assertNonEmpty(input.userId, 'userId');
  return utf8(`personal:userdek:${input.userId}`);
}

/**
 * AAD for the wrapped PCC DEK blob persisted as `PccKeyWrap.wrappedDek`.
 *
 *   pcc:dekwrap:v<version>
 */
export function aadForPccDekWrap(input: { readonly version: number }): Uint8Array {
  assertSafeKeyVersion(input.version, 'version');
  return utf8(`pcc:dekwrap:v${input.version}`);
}
