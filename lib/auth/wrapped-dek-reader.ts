// Greylock — wrapped per-user DEK reader (auth-side stub interface)
// =============================================================================
// AGENT-AUTH (Phase 2). The canonical domain `User` interface in
// `lib/types/domain.ts` does not expose `wrappedUserDek` (the wrapped per-user
// DEK is stored on the Prisma `User` row but stripped from the domain entity).
// `UserRepository.setWrappedUserDek` writes it; there is no symmetric reader.
//
// AuthService.completeAuthentication needs this value to call
// `CryptoService.loadUserDek`. We define the reader interface here (auth-side)
// so AGENT-DB can implement it without modifying the read-only `lib/types/*`
// contracts. Phase 3 retro will rationalize whether this should move into
// `lib/types/services.ts` or stay co-located with auth.
// =============================================================================

import type {
  EncryptedBlob,
  PasskeyId,
  RepoError,
  Result,
  UserId,
} from '../types/domain.js';

export interface WrappedDekReader {
  /** Read the active wrapped per-user DEK for `userId`. Returns `null` if the
   *  user exists but has not yet completed enrollment (no DEK row yet). */
  readWrappedUserDek(userId: UserId): Promise<Result<EncryptedBlob | null, RepoError>>;

  /** Read the per-credential `kekSalt` bytes. Stored on the Prisma `Passkey`
   *  row but stripped from the domain `Passkey` entity. AuthService needs it
   *  at login to derive the per-user KEK. */
  readPasskeyKekSalt(passkeyId: PasskeyId): Promise<Result<Uint8Array | null, RepoError>>;
}
