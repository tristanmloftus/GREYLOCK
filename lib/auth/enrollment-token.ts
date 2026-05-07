// Greylock — enrollment-token stub interface
// =============================================================================
// AGENT-AUTH owns this interface; AGENT-DB will implement the concrete
// repository in Phase 3 once the `EnrollmentToken` Prisma model is added.
//
// Why this lives in `lib/auth/` rather than `lib/types/services.ts`:
//   The orchestrator's brief instructed AGENT-AUTH not to add an
//   `EnrollmentTokenRepository` to the canonical type contract. We define the
//   stub here so the auth service factory can accept a dependency by interface,
//   and the Phase 3 implementer can either: (a) move this interface into
//   `lib/types/services.ts` once contracts are rationalized, or (b) keep it
//   colocated with auth.
//
// One-shot URL token contract (per docs/API.md §9 admin:enroll):
//   - Mint with random 32 bytes; SHA-256 the bytes; store the hash + email +
//     issuedAt + expiresAt + usedAt. URL carries the cleartext bytes (base64url).
//   - Verify by hashing the URL token, looking up the hash, asserting `usedAt`
//     is null and `expiresAt > now`.
//   - On successful complete, mark `usedAt = now` so the token can never replay.
// =============================================================================

import type { Result } from '../types/domain.js';

/** Domain shape of a verified token. We expose `email` so begin/complete can
 *  bind the ceremony to the same address; we never leak the hash bytes. */
export interface VerifiedEnrollmentToken {
  readonly tokenId: string;
  readonly email: string;
  readonly issuedAt: Date;
  readonly expiresAt: Date;
}

export type EnrollmentTokenError =
  | { readonly kind: 'token_not_found' }
  | { readonly kind: 'token_expired' }
  | { readonly kind: 'token_already_used' }
  | { readonly kind: 'storage_failure' };

export interface EnrollmentTokenRepository {
  /** Look up a token by its cleartext value. Performs the hash internally —
   *  callers MUST NOT hash before passing in. Returns the verified token,
   *  but does NOT mark it used (caller decides when to commit consumption). */
  verify(input: { readonly token: string }): Promise<Result<VerifiedEnrollmentToken, EnrollmentTokenError>>;

  /** Mark a token as consumed. Idempotent: marking an already-used token
   *  returns `token_already_used`. */
  consume(input: { readonly tokenId: string }): Promise<Result<void, EnrollmentTokenError>>;
}
