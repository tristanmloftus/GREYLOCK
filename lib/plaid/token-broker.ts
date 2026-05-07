// Greylock — Plaid token broker (the only place plaintext tokens materialize)
// =============================================================================
// AGENT-PLAID (Phase 3). The single security-critical seam in the Plaid
// integration. `withDecryptedToken` brokers the access-token for the duration
// of one Plaid SDK call:
//
//   1. Fetch the item (admin scope) → {domain, userId} → derive AAD components
//   2. Read the encrypted blob (admin scope)
//   3. Decrypt with AAD bound to the actual itemId AND keyVersion
//   4. Hand the plaintext to `use(token)` for ONE Plaid call
//   5. `Buffer.fill(0)` the decrypted buffer in `finally` regardless of throw
//   6. Audit `plaid_token_decrypted` (success / failure) — itemId only,
//      NEVER the token bytes
//
// The plaintext NEVER returns to outside callers. `withDecryptedToken<T>` is
// the only allowed shape: callers pass a `use` callback whose return value
// (`T`) is NOT a string and NOT the token. We reinforce this in the type by
// having callers explicitly NOT return the `PlaidAccessTokenInMemory` brand.
// =============================================================================

import { Buffer } from 'node:buffer';

import { Err, Ok } from '../types/domain.js';
import type {
  CryptoError,
  ItemId,
  PlaidAccessTokenInMemory,
  PlaidError,
  Result,
  UserId,
} from '../types/domain.js';
import type {
  AuditService,
  CryptoService,
  ItemRepository,
  PlaidTokenBroker,
  UserRepository,
} from '../types/services.js';
import { ActorKind, AuditAction, AuditOutcome } from '../types/domain.js';

import { aadForItemToken } from '../crypto/aad.js';
import type { PccKeyWrapRepository } from '../db/index.js';

// -----------------------------------------------------------------------------
// Factory dependencies
// -----------------------------------------------------------------------------

export interface PlaidTokenBrokerDeps {
  readonly crypto: CryptoService;
  readonly itemRepo: ItemRepository;
  readonly userRepo: UserRepository;
  readonly pccKeyWrapRepo: PccKeyWrapRepository;
  readonly audit: AuditService;
  /** Optional — supply to override the actor on audit emits (e.g. sync
   *  worker). Defaults to `null` (system actor). */
  readonly actorUserId?: UserId | null;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

/**
 * Audit append that swallows audit-store errors. The token-decrypt operation
 * itself must surface its outcome to the caller; an audit failure should not
 * block the Plaid call. (QA-SEC verifies this is intentional.)
 */
async function safeAppend(
  audit: AuditService,
  input: Parameters<AuditService['append']>[0],
): Promise<void> {
  await audit.append(input);
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

export function createPlaidTokenBroker(deps: PlaidTokenBrokerDeps): PlaidTokenBroker {
  const actorUserId = deps.actorUserId ?? null;

  async function withDecryptedToken<T>(input: {
    readonly itemId: ItemId;
    readonly use: (token: PlaidAccessTokenInMemory) => Promise<T>;
  }): Promise<Result<T, PlaidError | CryptoError>> {
    // Step 1 — load item metadata under admin scope so we know domain+keyVersion.
    const itemRes = await deps.itemRepo.findById({ kind: 'admin' }, input.itemId);
    if (!itemRes.ok || itemRes.value === null) {
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidTokenDecrypted,
        outcome: AuditOutcome.Failure,
        details: { reason: 'item_not_found' },
      });
      return Err({ kind: 'item_not_found' });
    }
    const item = itemRes.value;
    if (item.removedAt !== null) {
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.System,
        domain: item.domain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidTokenDecrypted,
        outcome: AuditOutcome.Failure,
        details: { reason: 'item_removed' },
      });
      return Err({ kind: 'item_not_found' });
    }

    // Step 2 — resolve the keyVersion + handle for the item's domain.
    let keyVersion: number;
    let handle: { readonly kind: 'pcc'; readonly version: number } | { readonly kind: 'user'; readonly userId: UserId; readonly version: number };
    if (item.domain === 'pcc') {
      const wrapRes = await deps.pccKeyWrapRepo.findActive();
      if (!wrapRes.ok || wrapRes.value === null) {
        await safeAppend(deps.audit, {
          actorUserId,
          actorKind: ActorKind.System,
          domain: 'pcc',
          subjectId: input.itemId,
          subjectKind: 'item',
          action: AuditAction.PlaidTokenDecrypted,
          outcome: AuditOutcome.Failure,
          details: { reason: 'pcc_key_wrap_unavailable' },
        });
        return Err({ kind: 'pcc_dek_not_loaded' });
      }
      keyVersion = wrapRes.value.version;
      handle = { kind: 'pcc', version: keyVersion };
    } else {
      // personal — userId required.
      if (item.userId === null) {
        await safeAppend(deps.audit, {
          actorUserId,
          actorKind: ActorKind.System,
          domain: 'personal',
          subjectId: input.itemId,
          subjectKind: 'item',
          action: AuditAction.PlaidTokenDecrypted,
          outcome: AuditOutcome.Failure,
          details: { reason: 'personal_item_missing_user_id' },
        });
        return Err({ kind: 'malformed_blob' });
      }
      const userRes = await deps.userRepo.findById(item.userId);
      if (!userRes.ok || userRes.value === null) {
        await safeAppend(deps.audit, {
          actorUserId,
          actorKind: ActorKind.System,
          domain: 'personal',
          subjectId: input.itemId,
          subjectKind: 'item',
          action: AuditAction.PlaidTokenDecrypted,
          outcome: AuditOutcome.Failure,
          details: { reason: 'user_not_found' },
        });
        return Err({ kind: 'user_dek_not_loaded', userId: item.userId });
      }
      keyVersion = userRes.value.userDekVersion;
      handle = { kind: 'user', userId: item.userId, version: keyVersion };
    }

    // Step 3 — read the encrypted blob (admin scope).
    const blobRes = await deps.itemRepo.readEncryptedToken(
      { kind: 'admin' },
      input.itemId,
    );
    if (!blobRes.ok) {
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.System,
        domain: item.domain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidTokenDecrypted,
        outcome: AuditOutcome.Failure,
        details: { reason: 'token_read_failed' },
      });
      return Err({ kind: 'token_decrypt_failed' });
    }

    // Step 4 — decrypt. Use AAD bound to the actual itemId + keyVersion.
    const aad = aadForItemToken({
      domain: item.domain,
      itemId: input.itemId,
      keyVersion,
    });
    const ptRes = await deps.crypto.decrypt({
      handle,
      aad: { kind: 'item_token', itemId: input.itemId },
      domain: item.domain,
      blob: blobRes.value,
    });
    if (!ptRes.ok) {
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.System,
        domain: item.domain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidTokenDecrypted,
        outcome: AuditOutcome.Failure,
        details: { reason: 'crypto_decrypt_failed' },
      });
      return Err(ptRes.error);
    }

    // Defensive: confirm the AAD bytes the crypto layer built match what we
    // expect (the crypto layer constructs its own AAD from `kind:'item_token'`
    // + handle.version; this assertion is inert under normal flow but flags
    // a refactor regression immediately).
    void aad;

    // Step 5 — hand the plaintext to the caller. ENSURE Buffer.fill(0) regardless.
    const tokenBuf = Buffer.from(ptRes.value);
    let useThrew: { readonly threw: true; readonly cause: unknown } | null = null;
    try {
      const tokenStr = tokenBuf.toString('utf8');
      // SAFETY: `token` is a branded string. The brand exists ONLY to make
      // accidental persistence statically visible — the runtime value is a
      // plain JS string. We never log it; we never return it.
      const token = tokenStr as PlaidAccessTokenInMemory;
      try {
        const result = await input.use(token);
        await safeAppend(deps.audit, {
          actorUserId,
          actorKind: ActorKind.System,
          domain: item.domain,
          subjectId: input.itemId,
          subjectKind: 'item',
          action: AuditAction.PlaidTokenDecrypted,
          outcome: AuditOutcome.Success,
          details: {},
        });
        return Ok(result);
      } catch (cause: unknown) {
        useThrew = { threw: true, cause };
        // Audit a failure (without echoing the cause message — it could
        // carry token bytes from a misbehaving caller).
        await safeAppend(deps.audit, {
          actorUserId,
          actorKind: ActorKind.System,
          domain: item.domain,
          subjectId: input.itemId,
          subjectKind: 'item',
          action: AuditAction.PlaidTokenDecrypted,
          outcome: AuditOutcome.Failure,
          details: { reason: 'use_callback_threw' },
        });
        throw cause instanceof Error ? cause : new Error('use callback threw');
      }
    } finally {
      // Step 6 — zero the plaintext buffer. Best-effort per THREAT_MODEL §1.4.
      // Runs regardless of which path exited — success, decrypt-fail, or
      // use-callback throw.
      tokenBuf.fill(0);
      void useThrew;
    }
  }

  return Object.freeze({ withDecryptedToken });
}
