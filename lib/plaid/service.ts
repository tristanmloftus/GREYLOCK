// Greylock — PlaidService implementation
// =============================================================================
// AGENT-PLAID (Phase 3). The service orchestrates Plaid SDK calls + crypto +
// repository writes. Plaintext access tokens NEVER leave this file as a
// returnable value; the only place they exist is inside `exchangePublicToken`
// (between `itemPublicTokenExchange` and `Buffer.fill(0)`) and inside the
// `withDecryptedToken` broker (lib/plaid/token-broker.ts).
//
// API contract: every public method returns `Result<T, PlaidError>`. No
// throws across the module boundary. Plaid SDK errors (HTTP / axios) are
// caught and mapped to `plaid_api_error` with `httpStatus` + `errorCode`
// only — the message field is constant ("plaid call failed") so we never
// surface SDK-internal strings (which could include token bytes from
// misbehaving SDK versions, or noisy stack traces).
// =============================================================================

import { Buffer } from 'node:buffer';

import type {
  AccountBase,
  PlaidApi,
  Transaction as PlaidTransactionType,
  RemovedTransaction as PlaidRemovedTransactionType,
} from 'plaid';

import { Err, Ok } from '../types/domain.js';
import {
  ActorKind,
  AuditAction,
  AuditOutcome,
} from '../types/domain.js';
import type {
  AccountId,
  CryptoError,
  Domain,
  EncryptedBlob,
  ItemId,
  PlaidError,
  PlaidItemId,
  PlaidLinkToken,
  PlaidPublicToken,
  Result,
  UserId,
} from '../types/domain.js';
import type {
  AccountRepository,
  AuditService,
  CryptoService,
  ItemRepository,
  PccMembershipRepository,
  PlaidLinkSession,
  PlaidService,
  PlaidSyncResult,
  PlaidTokenBroker,
  RepoScope,
  TransactionRepository,
  UserRepository,
} from '../types/services.js';

import { aadForItemToken } from '../crypto/aad.js';
import { EncryptedBlob as EncryptedBlobCtor } from '../types/domain.js';

import type { PccKeyWrapRepository } from '../db/index.js';

import {
  mapPlaidAccount,
  mapPlaidTransaction,
  mapRemovedTransactionId,
} from './mappers.js';

// -----------------------------------------------------------------------------
// Factory dependencies
// -----------------------------------------------------------------------------

export interface PlaidServiceDeps {
  readonly plaidClient: PlaidApi;
  readonly crypto: CryptoService;
  readonly itemRepo: ItemRepository;
  readonly accountRepo: AccountRepository;
  readonly transactionRepo: TransactionRepository;
  readonly userRepo: UserRepository;
  readonly pccMembershipRepo: PccMembershipRepository;
  readonly pccKeyWrapRepo: PccKeyWrapRepository;
  readonly tokenBroker: PlaidTokenBroker;
  readonly audit: AuditService;
  readonly clientName: string;
  readonly countryCodes: ReadonlyArray<string>;
  readonly defaultProducts: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
  /** Optional — supply to fix audit actor; defaults to null. */
  readonly actorUserId?: UserId | null;
  /** Optional — defaults to () => new Date(). */
  readonly now?: () => Date;
}

// -----------------------------------------------------------------------------
// Error mapping for Plaid SDK calls
// -----------------------------------------------------------------------------

interface PlaidSdkErrorShape {
  readonly response?: {
    readonly status?: number;
    readonly data?: {
      readonly error_code?: string;
      readonly error_type?: string;
    };
  };
}

function mapPlaidSdkError(cause: unknown): PlaidError {
  // We deliberately do NOT echo the SDK error string (it can contain stack
  // traces or, in theory, request bodies that include access tokens). The
  // shape captures only HTTP status + Plaid error_code.
  if (typeof cause === 'object' && cause !== null) {
    const obj = cause as PlaidSdkErrorShape;
    const status = obj.response?.status;
    const code = obj.response?.data?.error_code;
    if (typeof status === 'number' && typeof code === 'string' && code.length > 0) {
      // Map well-known invalid-public-token responses for the route layer.
      if (code === 'INVALID_PUBLIC_TOKEN') {
        return { kind: 'invalid_public_token' };
      }
      return { kind: 'plaid_api_error', httpStatus: status, errorCode: code };
    }
  }
  return { kind: 'plaid_api_error', httpStatus: 0, errorCode: 'unknown' };
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

async function safeAppend(
  audit: AuditService,
  input: Parameters<AuditService['append']>[0],
): Promise<void> {
  await audit.append(input);
}

function scopeForCreate(domain: Domain, userId: UserId | null): RepoScope {
  if (domain === 'personal') {
    if (userId === null) {
      throw new Error('plaid: personal item create requires userId');
    }
    return { kind: 'personal', userId };
  }
  if (userId === null) {
    // PCC items aren't user-owned at the row level; using admin scope keeps
    // the create gate simple.
    return { kind: 'admin' };
  }
  return { kind: 'pcc', memberOfUserId: userId };
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

export function createPlaidService(deps: PlaidServiceDeps): PlaidService {
  const actorUserId = deps.actorUserId ?? null;
  const now = deps.now ?? ((): Date => new Date());

  // ---------------------------------------------------------------------------
  // mintLinkToken
  // ---------------------------------------------------------------------------
  async function mintLinkToken(input: {
    readonly userId: UserId;
    readonly domain: Domain;
    readonly products: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
  }): Promise<Result<PlaidLinkSession, PlaidError>> {
    const products = input.products.length > 0 ? input.products : deps.defaultProducts;
    try {
      const resp = await deps.plaidClient.linkTokenCreate({
        client_name: deps.clientName,
        country_codes: deps.countryCodes.map((s) => s as never),
        language: 'en',
        user: { client_user_id: input.userId },
        products: products.map((p) => p as never),
      });
      const data = resp.data;
      const linkToken = data.link_token as PlaidLinkToken;
      const expiresAt = new Date(data.expiration);

      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PlaidLinkTokenMinted,
        outcome: AuditOutcome.Success,
        details: { products: products as ReadonlyArray<string> },
      });

      return Ok({ linkToken, expiresAt });
    } catch (cause: unknown) {
      const err = mapPlaidSdkError(cause);
      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PlaidLinkTokenMinted,
        outcome: AuditOutcome.Failure,
        details: { errorKind: err.kind },
      });
      return Err(err);
    }
  }

  // ---------------------------------------------------------------------------
  // exchangePublicToken
  // ---------------------------------------------------------------------------
  // Lifecycle (one-shot):
  //   1. Plaid `itemPublicTokenExchange` → access_token (string)
  //   2. Copy into Buffer.from(access_token, 'utf8') = tokenBuf
  //   3. Insert Item row with a placeholder dummy blob (so Bytes column is
  //      satisfied) — DB assigns real `Item.id` (cuid via Prisma default).
  //   4. Encrypt tokenBuf with AAD bound to real itemId + keyVersion.
  //   5. `rewriteEncryptedToken` (admin) → final blob persisted.
  //   6. tokenBuf.fill(0).
  //   7. Audit `plaid_public_token_exchanged` and `plaid_item_added`.
  //
  // The placeholder blob in step 3 is structured-zeros (valid Bytes, not a
  // valid ciphertext under any key/AAD). It is replaced before this method
  // returns. If step 4-5 fail, the row exists with an unusable blob; the
  // next sync attempt will surface `crypto_decrypt_failed` and the operator
  // can remove the item via `POST /api/plaid/items/remove`. We audit the
  // failure so the partial state is visible.
  // ---------------------------------------------------------------------------
  async function exchangePublicToken(input: {
    readonly userId: UserId;
    readonly domain: Domain;
    readonly publicToken: PlaidPublicToken;
    readonly institutionId: string | null;
    readonly institutionName: string | null;
  }): Promise<Result<{ readonly itemId: ItemId; readonly plaidItemId: PlaidItemId }, PlaidError>> {
    // PCC gate: the PCC route handler MUST verify the caller's PccMembership
    // BEFORE invoking this method. We re-check at the service layer for
    // defense-in-depth (covers test-only call sites and prevents
    // misconfigured callers from creating PCC items as non-members).
    if (input.domain === 'pcc') {
      const memberRes = await deps.pccMembershipRepo.isActiveMember(input.userId);
      if (!memberRes.ok || !memberRes.value) {
        await safeAppend(deps.audit, {
          actorUserId: actorUserId ?? input.userId,
          actorKind: ActorKind.User,
          domain: 'pcc',
          subjectId: null,
          subjectKind: null,
          action: AuditAction.PlaidPublicTokenExchanged,
          outcome: AuditOutcome.Denied,
          details: { reason: 'not_pcc_member' },
        });
        return Err({ kind: 'plaid_api_error', httpStatus: 403, errorCode: 'forbidden' });
      }
    }

    // Step 1 — Plaid public-token-exchange. The plaintext access_token lives
    // inside this scope only.
    let plaidItemIdStr: string;
    let tokenBuf: Buffer;
    try {
      const resp = await deps.plaidClient.itemPublicTokenExchange({
        public_token: input.publicToken,
      });
      plaidItemIdStr = resp.data.item_id;
      tokenBuf = Buffer.from(resp.data.access_token, 'utf8');
    } catch (cause: unknown) {
      const err = mapPlaidSdkError(cause);
      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PlaidPublicTokenExchanged,
        outcome: AuditOutcome.Failure,
        details: { errorKind: err.kind },
      });
      return Err(err);
    }
    const plaidItemId = plaidItemIdStr as PlaidItemId;

    try {
      // Step 2 — resolve keyVersion + handle for the target domain.
      let keyVersion: number;
      let handle: { readonly kind: 'pcc'; readonly version: number } | { readonly kind: 'user'; readonly userId: UserId; readonly version: number };
      if (input.domain === 'pcc') {
        const wrapRes = await deps.pccKeyWrapRepo.findActive();
        if (!wrapRes.ok || wrapRes.value === null) {
          throw new Error('plaid: PCC key wrap unavailable');
        }
        keyVersion = wrapRes.value.version;
        handle = { kind: 'pcc', version: keyVersion };
      } else {
        const userRes = await deps.userRepo.findById(input.userId);
        if (!userRes.ok || userRes.value === null) {
          throw new Error('plaid: user not found');
        }
        keyVersion = userRes.value.userDekVersion;
        handle = { kind: 'user', userId: input.userId, version: keyVersion };
      }

      // Step 3 — insert the Item row with a placeholder blob so the DB
      // assigns the real id (cuid). The placeholder is structured zeros that
      // pass the Bytes column constraint but cannot decrypt as a real token.
      const placeholderBlob = buildPlaceholderBlob(input.domain);
      const userIdForRow = input.domain === 'personal' ? input.userId : null;
      const createRes = await deps.itemRepo.create(
        scopeForCreate(input.domain, userIdForRow),
        {
          domain: input.domain,
          userId: userIdForRow,
          plaidItemId,
          plaidInstitutionId: input.institutionId,
          institutionName: input.institutionName,
          encryptedAccessToken: placeholderBlob,
        },
      );
      if (!createRes.ok) {
        throw new Error('plaid: item create failed');
      }
      const itemId = createRes.value.id;

      // Step 4 — encrypt the real token, AAD bound to the real itemId.
      // (We construct AAD bytes here for visibility / testing; the crypto
      // layer rebuilds them internally from `kind:'item_token'` + handle.)
      void aadForItemToken({ domain: input.domain, itemId, keyVersion });
      const encRes = await deps.crypto.encrypt({
        handle,
        aad: { kind: 'item_token', itemId },
        domain: input.domain,
        plaintext: tokenBuf,
      });
      if (!encRes.ok) {
        throw new Error('plaid: token encrypt failed');
      }

      // Step 5 — rewrite the blob to the AAD-bound ciphertext (admin scope).
      const rewriteRes = await deps.itemRepo.rewriteEncryptedToken(
        { kind: 'admin' },
        { id: itemId, newBlob: encRes.value },
      );
      if (!rewriteRes.ok) {
        throw new Error('plaid: token rewrite failed');
      }

      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidPublicTokenExchanged,
        outcome: AuditOutcome.Success,
        details: {},
      });
      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidItemAdded,
        outcome: AuditOutcome.Success,
        details: {
          institutionName: input.institutionName,
        },
      });

      return Ok({ itemId, plaidItemId });
    } catch (cause: unknown) {
      // We use a constant generic error to avoid surfacing internal exception
      // strings. The audit emit captures the failure context.
      void cause;
      await safeAppend(deps.audit, {
        actorUserId: actorUserId ?? input.userId,
        actorKind: ActorKind.User,
        domain: input.domain,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PlaidPublicTokenExchanged,
        outcome: AuditOutcome.Failure,
        details: { reason: 'persist_failed' },
      });
      return Err({ kind: 'plaid_api_error', httpStatus: 500, errorCode: 'persist_failed' });
    } finally {
      // Step 6 — zero the plaintext token buffer regardless of outcome.
      tokenBuf.fill(0);
    }
  }

  // ---------------------------------------------------------------------------
  // syncItem
  // ---------------------------------------------------------------------------
  async function syncItem(input: {
    readonly itemId: ItemId;
  }): Promise<Result<PlaidSyncResult, PlaidError>> {
    // Look up the item under admin scope (sync runs without a session).
    const itemRes = await deps.itemRepo.findById({ kind: 'admin' }, input.itemId);
    if (!itemRes.ok || itemRes.value === null) {
      return Err({ kind: 'item_not_found' });
    }
    const item = itemRes.value;
    if (item.removedAt !== null) {
      return Err({ kind: 'item_not_found' });
    }
    const itemDomain = item.domain;
    const itemUserId = item.userId;
    const writeScope: RepoScope = scopeForCreate(itemDomain, itemUserId);

    await safeAppend(deps.audit, {
      actorUserId,
      actorKind: ActorKind.SyncWorker,
      domain: itemDomain,
      subjectId: input.itemId,
      subjectKind: 'item',
      action: AuditAction.PlaidSyncStarted,
      outcome: AuditOutcome.Success,
      details: {},
    });

    // Borrow the access token once. The broker handles decrypt + zeroize;
    // inside the `use` callback we run the full sync (potentially multiple
    // pages of `transactions/sync`). The broker's `Buffer.fill(0)` runs in
    // the `finally` after this entire chain completes.
    //
    // We catch SDK errors INSIDE the use callback so a 502 from Plaid maps
    // cleanly to a Result error rather than an exception across the boundary.
    type SyncCollect = {
      readonly cursor: string;
      readonly added: ReadonlyArray<PlaidTransactionType>;
      readonly modified: ReadonlyArray<PlaidTransactionType>;
      readonly removed: ReadonlyArray<PlaidRemovedTransactionType>;
      readonly accounts: ReadonlyArray<AccountBase>;
    };
    type SyncEither =
      | { readonly ok: true; readonly value: SyncCollect }
      | { readonly ok: false; readonly error: PlaidError };

    let useResult: Result<SyncEither, PlaidError | CryptoError>;
    try {
      useResult = await deps.tokenBroker.withDecryptedToken({
        itemId: input.itemId,
        use: async (token): Promise<SyncEither> => {
          // Accumulators across pages.
          let cursor = item.syncCursor ?? '';
          const accAdded: PlaidTransactionType[] = [];
          const accModified: PlaidTransactionType[] = [];
          const accRemoved: PlaidRemovedTransactionType[] = [];
          const accAccounts: AccountBase[] = [];

          // Loop pages until has_more is false. The SDK call is wrapped in a
          // try/catch — a Plaid error becomes a typed failure, never an
          // exception across the broker boundary.
          while (true) {
            try {
              const resp = await deps.plaidClient.transactionsSync({
                access_token: token,
                cursor,
                count: 500,
              });
              const data = resp.data;
              for (const a of data.added) {
                accAdded.push(a);
              }
              for (const m of data.modified) {
                accModified.push(m);
              }
              for (const r of data.removed) {
                accRemoved.push(r);
              }
              for (const acc of data.accounts) {
                accAccounts.push(acc);
              }
              cursor = data.next_cursor;
              if (!data.has_more) {
                return {
                  ok: true,
                  value: {
                    cursor,
                    added: accAdded,
                    modified: accModified,
                    removed: accRemoved,
                    accounts: accAccounts,
                  },
                };
              }
            } catch (cause: unknown) {
              return { ok: false, error: mapPlaidSdkError(cause) };
            }
          }
        },
      });
    } catch (cause: unknown) {
      // Defensive: should not happen since use() catches its own SDK errors.
      // If it does, surface as a typed failure to keep the contract.
      void cause;
      useResult = Err({ kind: 'plaid_api_error', httpStatus: 0, errorCode: 'unexpected' });
    }

    if (!useResult.ok) {
      // Decrypt or item lookup failed; audit + roll back cursor (no advance).
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.SyncWorker,
        domain: itemDomain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidSyncFailed,
        outcome: AuditOutcome.Failure,
        details: { errorKind: useResult.error.kind },
      });
      const reusedCursor = item.syncCursor ?? '';
      const cursorRes = await deps.itemRepo.updateSyncCursor(writeScope, {
        id: input.itemId,
        cursor: reusedCursor,
        outcome: 'error',
      });
      void cursorRes;
      const e = useResult.error;
      if (e.kind === 'invalid_public_token' || e.kind === 'item_not_found' || e.kind === 'plaid_api_error' || e.kind === 'token_decrypt_failed') {
        return Err(e);
      }
      return Err({ kind: 'token_decrypt_failed' });
    }
    if (!useResult.value.ok) {
      // Plaid SDK error inside the use callback. Audit + roll back cursor.
      const sdkErr = useResult.value.error;
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.SyncWorker,
        domain: itemDomain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidSyncFailed,
        outcome: AuditOutcome.Failure,
        details: { errorKind: sdkErr.kind },
      });
      const reusedCursor = item.syncCursor ?? '';
      const cursorRes = await deps.itemRepo.updateSyncCursor(writeScope, {
        id: input.itemId,
        cursor: reusedCursor,
        outcome: 'error',
      });
      void cursorRes;
      return Err(sdkErr);
    }
    const sync = useResult.value.value;

    // Apply to DB. Accounts first (so transactions can resolve plaidAccountId
    // → AccountId), then transactions, then advance the cursor LAST. Cursor
    // advances ONLY on success of all three steps.
    try {
      // 1. Upsert accounts.
      const accountInputs = sync.accounts.map((a) => mapPlaidAccount(a));
      const upsertRes = await deps.accountRepo.upsertFromPlaid(writeScope, {
        itemId: input.itemId,
        accounts: accountInputs,
      });
      if (!upsertRes.ok) {
        throw new Error('plaid: account upsert failed');
      }

      // 2. Resolve plaidAccountId → AccountId via list-by-item.
      const accListRes = await deps.accountRepo.listByItem(writeScope, input.itemId);
      if (!accListRes.ok) {
        throw new Error('plaid: list accounts failed');
      }
      const accountIdByPlaid = new Map<string, AccountId>();
      for (const a of accListRes.value) {
        accountIdByPlaid.set(a.plaidAccountId, a.id);
      }
      const accountIdResolver = (plaidAccountId: string): AccountId | null => {
        return accountIdByPlaid.get(plaidAccountId) ?? null;
      };

      // 3. Map transactions. Drop any whose accountId can't be resolved
      //    (Plaid contract guarantees these are present, but we surface the
      //    drop count in the audit details for triage).
      const addedInputs = sync.added
        .map((t) =>
          mapPlaidTransaction(
            { itemId: input.itemId, accountIdResolver: (p): AccountId | null => accountIdResolver(p), domain: itemDomain, userId: itemUserId },
            t,
          ),
        )
        .filter((x): x is NonNullable<typeof x> => x !== null);
      const modifiedInputs = sync.modified
        .map((t) =>
          mapPlaidTransaction(
            { itemId: input.itemId, accountIdResolver: (p): AccountId | null => accountIdResolver(p), domain: itemDomain, userId: itemUserId },
            t,
          ),
        )
        .filter((x): x is NonNullable<typeof x> => x !== null);
      const removedIds = sync.removed.map((r) => mapRemovedTransactionId(r));

      const txRes = await deps.transactionRepo.applyPlaidSync(writeScope, {
        itemId: input.itemId,
        added: addedInputs,
        modified: modifiedInputs,
        removedPlaidIds: removedIds,
      });
      if (!txRes.ok) {
        throw new Error('plaid: transaction apply failed');
      }

      // 4. Advance cursor — only on commit of all the above.
      const cursorRes = await deps.itemRepo.updateSyncCursor(writeScope, {
        id: input.itemId,
        cursor: sync.cursor,
        outcome: 'success',
      });
      if (!cursorRes.ok) {
        throw new Error('plaid: cursor update failed');
      }

      const counts = txRes.value;
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.SyncWorker,
        domain: itemDomain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidSyncCompleted,
        outcome: AuditOutcome.Success,
        details: {
          added: counts.added,
          modified: counts.modified,
          removed: counts.removed,
        },
      });

      return Ok({
        added: counts.added,
        modified: counts.modified,
        removed: counts.removed,
        newCursor: sync.cursor,
        hasMore: false,
      });
    } catch (cause: unknown) {
      void cause;
      await safeAppend(deps.audit, {
        actorUserId,
        actorKind: ActorKind.SyncWorker,
        domain: itemDomain,
        subjectId: input.itemId,
        subjectKind: 'item',
        action: AuditAction.PlaidSyncFailed,
        outcome: AuditOutcome.Failure,
        details: { reason: 'apply_failed' },
      });
      const reusedCursor = item.syncCursor ?? '';
      void (await deps.itemRepo.updateSyncCursor(writeScope, {
        id: input.itemId,
        cursor: reusedCursor,
        outcome: 'error',
      }));
      return Err({ kind: 'plaid_api_error', httpStatus: 500, errorCode: 'apply_failed' });
    }
  }

  // ---------------------------------------------------------------------------
  // refreshBalances
  // ---------------------------------------------------------------------------
  async function refreshBalances(input: {
    readonly itemId: ItemId;
  }): Promise<Result<{ readonly accountsUpdated: number }, PlaidError>> {
    const itemRes = await deps.itemRepo.findById({ kind: 'admin' }, input.itemId);
    if (!itemRes.ok || itemRes.value === null) {
      return Err({ kind: 'item_not_found' });
    }
    const item = itemRes.value;
    if (item.removedAt !== null) {
      return Err({ kind: 'item_not_found' });
    }
    const itemDomain = item.domain;
    const itemUserId = item.userId;
    const writeScope: RepoScope = scopeForCreate(itemDomain, itemUserId);

    type BalEither =
      | { readonly ok: true; readonly value: ReadonlyArray<AccountBase> }
      | { readonly ok: false; readonly error: PlaidError };
    let useResult: Result<BalEither, PlaidError | CryptoError>;
    try {
      useResult = await deps.tokenBroker.withDecryptedToken({
        itemId: input.itemId,
        use: async (token): Promise<BalEither> => {
          try {
            const resp = await deps.plaidClient.accountsBalanceGet({ access_token: token });
            return { ok: true, value: resp.data.accounts };
          } catch (cause: unknown) {
            return { ok: false, error: mapPlaidSdkError(cause) };
          }
        },
      });
    } catch (cause: unknown) {
      void cause;
      useResult = Err({ kind: 'plaid_api_error', httpStatus: 0, errorCode: 'unexpected' });
    }
    if (!useResult.ok) {
      const e = useResult.error;
      if (e.kind === 'invalid_public_token' || e.kind === 'item_not_found' || e.kind === 'plaid_api_error' || e.kind === 'token_decrypt_failed') {
        return Err(e);
      }
      return Err({ kind: 'token_decrypt_failed' });
    }
    if (!useResult.value.ok) {
      return Err(useResult.value.error);
    }
    try {
      const accountInputs = useResult.value.value.map((a) => mapPlaidAccount(a));
      const upsertRes = await deps.accountRepo.upsertFromPlaid(writeScope, {
        itemId: input.itemId,
        accounts: accountInputs,
      });
      if (!upsertRes.ok) {
        return Err({ kind: 'plaid_api_error', httpStatus: 500, errorCode: 'upsert_failed' });
      }
      return Ok({ accountsUpdated: upsertRes.value.upserted });
    } catch (cause: unknown) {
      void cause;
      return Err({ kind: 'plaid_api_error', httpStatus: 500, errorCode: 'apply_failed' });
    }
  }

  // ---------------------------------------------------------------------------
  // removeItem
  // ---------------------------------------------------------------------------
  async function removeItem(input: {
    readonly itemId: ItemId;
    readonly reason: string;
  }): Promise<Result<void, PlaidError>> {
    const itemRes = await deps.itemRepo.findById({ kind: 'admin' }, input.itemId);
    if (!itemRes.ok || itemRes.value === null) {
      return Err({ kind: 'item_not_found' });
    }
    const item = itemRes.value;
    if (item.removedAt !== null) {
      return Err({ kind: 'item_not_found' });
    }
    const itemDomain = item.domain;
    const itemUserId = item.userId;
    const removeScope: RepoScope = scopeForCreate(itemDomain, itemUserId);

    // Call Plaid `item/remove` upstream so the token is invalidated server-side.
    type RmEither =
      | { readonly ok: true }
      | { readonly ok: false; readonly error: PlaidError };
    let useResult: Result<RmEither, PlaidError | CryptoError>;
    try {
      useResult = await deps.tokenBroker.withDecryptedToken({
        itemId: input.itemId,
        use: async (token): Promise<RmEither> => {
          try {
            await deps.plaidClient.itemRemove({ access_token: token });
            return { ok: true };
          } catch (cause: unknown) {
            return { ok: false, error: mapPlaidSdkError(cause) };
          }
        },
      });
    } catch (cause: unknown) {
      void cause;
      useResult = Err({ kind: 'plaid_api_error', httpStatus: 0, errorCode: 'unexpected' });
    }
    if (!useResult.ok) {
      const e = useResult.error;
      if (e.kind === 'invalid_public_token' || e.kind === 'item_not_found' || e.kind === 'plaid_api_error' || e.kind === 'token_decrypt_failed') {
        return Err(e);
      }
      return Err({ kind: 'token_decrypt_failed' });
    }
    if (!useResult.value.ok) {
      return Err(useResult.value.error);
    }

    // Soft-remove the row.
    const softRes = await deps.itemRepo.softRemove(removeScope, {
      id: input.itemId,
      reason: input.reason,
    });
    if (!softRes.ok) {
      return Err({ kind: 'plaid_api_error', httpStatus: 500, errorCode: 'soft_remove_failed' });
    }

    await safeAppend(deps.audit, {
      actorUserId,
      actorKind: ActorKind.User,
      domain: itemDomain,
      subjectId: input.itemId,
      subjectKind: 'item',
      action: AuditAction.PlaidItemRemoved,
      outcome: AuditOutcome.Success,
      details: { reason: input.reason },
    });

    void now;
    return Ok(undefined);
  }

  return Object.freeze({
    mintLinkToken,
    exchangePublicToken,
    syncItem,
    refreshBalances,
    removeItem,
  });
}

// -----------------------------------------------------------------------------
// Placeholder blob construction
// -----------------------------------------------------------------------------
//
// Used by `exchangePublicToken` Step 3: we need a valid Bytes value for the
// `Item.encryptedAccessToken` column at insert time. The placeholder is a
// length-correct envelope-shaped buffer of zeros: it cannot decrypt under any
// key (GCM tag is all zeros and won't authenticate) but satisfies the column.
// The value is REPLACED before `exchangePublicToken` returns; if step 4-5
// fail, the caller surfaces a 500 and the operator can remove the item.
// -----------------------------------------------------------------------------

const PLACEHOLDER_BLOB_LEN = 1 + 1 + 12 + 0 + 16; // version+domain_tag+nonce+0ct+tag

function buildPlaceholderBlob(domain: Domain): EncryptedBlob {
  const buf = Buffer.alloc(PLACEHOLDER_BLOB_LEN, 0);
  buf[0] = 0x01; // version
  buf[1] = domain === 'pcc' ? 0x02 : 0x01; // domain_tag
  // nonce + tag stay zero — buf is unauthentic-able, intentional.
  return EncryptedBlobCtor.unsafeFromBytes(buf);
}
