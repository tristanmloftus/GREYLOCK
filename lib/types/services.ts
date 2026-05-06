// Greylock — service interface contracts
// =============================================================================
// AGENT-ARCH (Phase 1). Every cross-module contract lives here. No
// implementation. Phase 2-4 agents implement these interfaces; route handlers
// depend on these interfaces, not on concrete classes.
// =============================================================================

import type {
  Account,
  AccountId,
  ActorKind,
  AuditAction,
  AuditEntry,
  AuditError,
  AuditOutcome,
  AuditSeq,
  AuthError,
  BillionProgressResult,
  Cents,
  CryptoError,
  Domain,
  EncryptedBlob,
  IsoCurrencyCode,
  Item,
  ItemId,
  MonthNetResult,
  NetWorthResult,
  NetWorthSnapshot,
  Passkey,
  PasskeyId,
  PccMembership,
  PlaidAccessTokenInMemory,
  PlaidAccountId,
  PlaidError,
  PlaidItemId,
  PlaidLinkToken,
  PlaidPublicToken,
  PlaidTransactionId,
  RepoError,
  Result,
  Role,
  Session,
  SessionId,
  SubjectKind,
  Transaction,
  User,
  UserId,
} from './domain.js';

// -----------------------------------------------------------------------------
// CryptoService
// -----------------------------------------------------------------------------
// Implementations live in `lib/crypto/`. Three key tiers:
//   - Master KEK    (process memory; loaded at boot from Keychain passphrase)
//   - PCC DEK       (process memory; unwrapped from Master KEK at boot)
//   - per-user DEK  (process memory; unwrapped at session establishment;
//                    zeroized at logout / idle timeout)
//
// AAD scheme (binding plaintext to row identity + domain):
//   personal item token : utf8("personal:itemtoken:" + itemId + ":" + userDekVersion)
//   pcc item token      : utf8("pcc:itemtoken:" + itemId + ":" + masterKekVersion)
//   per-user DEK wrap   : utf8("personal:userdek:" + userId)
//   pcc DEK wrap        : utf8("pcc:dekwrap:v" + version)
// =============================================================================

/** Identifier of which in-memory key should be used to encrypt/decrypt. */
export type KeyHandle =
  | { readonly kind: 'pcc'; readonly version: number }
  | { readonly kind: 'user'; readonly userId: UserId; readonly version: number };

/** Stable identifier of the row whose payload is being protected. */
export type AadContext =
  | { readonly kind: 'item_token'; readonly itemId: ItemId }
  | { readonly kind: 'user_dek_wrap'; readonly userId: UserId }
  | { readonly kind: 'pcc_dek_wrap'; readonly version: number };

export interface CryptoService {
  /** Idempotent. Loads Master KEK + PCC DEK into memory. */
  initializeFromKeychain(): Promise<Result<void, CryptoError>>;

  /** Wipes Master KEK + PCC DEK from memory. Used on shutdown / panic. */
  shutdown(): Promise<void>;

  /** Derive + load a per-user DEK from a successful WebAuthn assertion. */
  loadUserDek(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly wrappedUserDek: EncryptedBlob;
    readonly userDekVersion: number;
  }): Promise<Result<void, CryptoError>>;

  /** Zeroize a single user's DEK from memory. */
  unloadUserDek(userId: UserId): Promise<void>;

  /** True iff `loadUserDek` has been called and not unloaded. */
  hasUserDek(userId: UserId): boolean;

  /** True iff Master KEK + PCC DEK are loaded. */
  hasPccDek(): boolean;

  /** Encrypt with the chosen key. Caller must specify domain via the handle. */
  encrypt(input: {
    readonly handle: KeyHandle;
    readonly aad: AadContext;
    readonly domain: Domain;
    readonly plaintext: Uint8Array;
  }): Promise<Result<EncryptedBlob, CryptoError>>;

  /** Decrypt with the chosen key. Mismatched domain or AAD => `aad_mismatch`. */
  decrypt(input: {
    readonly handle: KeyHandle;
    readonly aad: AadContext;
    readonly domain: Domain;
    readonly blob: EncryptedBlob;
  }): Promise<Result<Uint8Array, CryptoError>>;

  /** Wrap raw 32-byte DEK material into a `wrappedUserDek` blob. */
  wrapUserDek(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly version: number;
    readonly dekMaterial: Uint8Array;
  }): Promise<Result<EncryptedBlob, CryptoError>>;

  /** Bump userDekVersion + re-wrap. Caller is responsible for re-encrypting
   * dependent ciphertext (item tokens). */
  rotateUserDek(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly kekSalt: Uint8Array;
    readonly currentVersion: number;
  }): Promise<Result<{ readonly newVersion: number; readonly wrapped: EncryptedBlob }, CryptoError>>;

  /** Master-passphrase rotation: read old Master KEK, decrypt PCC DEK,
   * re-encrypt under new Master KEK, write new PccKeyWrap row, re-encrypt
   * every PCC item token. Atomic — either the whole rotation succeeds or
   * the previous wrap stays in use. */
  rotateMaster(): Promise<Result<{ readonly oldVersion: number; readonly newVersion: number }, CryptoError>>;
}

// -----------------------------------------------------------------------------
// AuthService
// -----------------------------------------------------------------------------
// Implementation in `lib/auth/`. Wraps `@simplewebauthn/server` and iron-session.
// =============================================================================

export interface AuthRegistrationOptions {
  readonly challenge: string;
  readonly rp: { readonly id: string; readonly name: string };
  readonly user: { readonly id: string; readonly name: string; readonly displayName: string };
  readonly pubKeyCredParams: ReadonlyArray<{ readonly type: 'public-key'; readonly alg: number }>;
  readonly timeout: number;
  readonly attestation: 'none';
  readonly authenticatorSelection: {
    readonly residentKey: 'required';
    readonly userVerification: 'required';
  };
}

export interface AuthAuthenticationOptions {
  readonly challenge: string;
  readonly rpId: string;
  readonly timeout: number;
  readonly userVerification: 'required';
  readonly allowCredentials: ReadonlyArray<{ readonly id: string; readonly type: 'public-key' }>;
}

export interface RegistrationResponseFromBrowser {
  // Output of `@simplewebauthn/browser#startRegistration`. Treated opaquely
  // by AuthService except for type-safety; fully validated by SimpleWebAuthn.
  readonly id: string;
  readonly rawId: string;
  readonly response: {
    readonly attestationObject: string;
    readonly clientDataJSON: string;
    readonly transports?: ReadonlyArray<string>;
  };
  readonly clientExtensionResults: Readonly<Record<string, unknown>>;
  readonly type: 'public-key';
  readonly authenticatorAttachment?: string;
}

export interface AuthenticationResponseFromBrowser {
  readonly id: string;
  readonly rawId: string;
  readonly response: {
    readonly authenticatorData: string;
    readonly clientDataJSON: string;
    readonly signature: string;
    readonly userHandle?: string;
  };
  readonly clientExtensionResults: Readonly<Record<string, unknown>>;
  readonly type: 'public-key';
  readonly authenticatorAttachment?: string;
}

export interface AuthService {
  /** Generate a registration challenge. Email MUST be in the allowlist and
   * MUST NOT be the placeholder. */
  beginEnrollment(input: {
    readonly email: string;
    readonly displayName: string;
    readonly role: Role;
  }): Promise<Result<AuthRegistrationOptions, AuthError>>;

  /** Verify the attestation, write Passkey row, derive + wrap user DEK. */
  completeEnrollment(input: {
    readonly email: string;
    readonly response: RegistrationResponseFromBrowser;
    readonly expectedChallenge: string;
    readonly deviceLabel: string | null;
  }): Promise<Result<{ readonly userId: UserId; readonly passkeyId: PasskeyId }, AuthError>>;

  /** Generate an authentication challenge for a known email. */
  beginAuthentication(input: { readonly email: string }): Promise<Result<AuthAuthenticationOptions, AuthError>>;

  /** Verify the assertion, revoke any prior active session, create new one,
   * load user DEK into CryptoService. */
  completeAuthentication(input: {
    readonly email: string;
    readonly response: AuthenticationResponseFromBrowser;
    readonly expectedChallenge: string;
    readonly userAgent: string | null;
    readonly remoteAddr: string | null;
  }): Promise<Result<{ readonly userId: UserId; readonly sessionId: SessionId; readonly cookieValue: string }, AuthError>>;

  /** Validate a session id from the cookie. Sliding-window updates `lastActivityAt`. */
  validateSession(input: { readonly sessionId: SessionId; readonly cookieValue: string }): Promise<Result<Session, AuthError>>;

  /** Revoke a session, unload its user's DEK if it was the last active. */
  revokeSession(input: { readonly sessionId: SessionId; readonly reason: string }): Promise<Result<void, AuthError>>;

  /** Revoke all active sessions; used by `pnpm admin:revoke-all`. */
  revokeAllSessions(input: { readonly reason: string }): Promise<Result<{ readonly count: number }, AuthError>>;
}

// -----------------------------------------------------------------------------
// PlaidService
// -----------------------------------------------------------------------------
// Implementation in `lib/plaid/`. Wraps the `plaid` SDK. NEVER exposes a
// plaintext access_token to callers — all API methods take an `ItemId` and
// internally fetch + decrypt the token, use it, and zeroize the buffer.
// =============================================================================

export interface PlaidLinkSession {
  readonly linkToken: PlaidLinkToken;
  readonly expiresAt: Date;
}

export interface PlaidSyncResult {
  readonly added: number;
  readonly modified: number;
  readonly removed: number;
  readonly newCursor: string;
  readonly hasMore: boolean;
}

export interface PlaidService {
  /** Mint a link token for the given user/domain. Domain controls which
   * institution data eventually gets encrypted under which key. */
  mintLinkToken(input: {
    readonly userId: UserId;
    readonly domain: Domain;
    readonly products: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
  }): Promise<Result<PlaidLinkSession, PlaidError>>;

  /** Exchange public_token, encrypt access_token under the right key, persist Item. */
  exchangePublicToken(input: {
    readonly userId: UserId;
    readonly domain: Domain;
    readonly publicToken: PlaidPublicToken;
    readonly institutionId: string | null;
    readonly institutionName: string | null;
  }): Promise<Result<{ readonly itemId: ItemId; readonly plaidItemId: PlaidItemId }, PlaidError>>;

  /** Run `transactions/sync` for an item; updates cursor + writes Account/Tx rows. */
  syncItem(input: { readonly itemId: ItemId }): Promise<Result<PlaidSyncResult, PlaidError>>;

  /** Fetch latest account balances; updates Account.currentBalanceCents. */
  refreshBalances(input: { readonly itemId: ItemId }): Promise<Result<{ readonly accountsUpdated: number }, PlaidError>>;

  /** Call `item/remove` upstream; soft-delete the row; zeroize encrypted token. */
  removeItem(input: { readonly itemId: ItemId; readonly reason: string }): Promise<Result<void, PlaidError>>;
}

/**
 * Internal-only helper. Implemented inside `lib/plaid/` and never exported.
 * The shape exists here to make threat-model traces explicit: the only place
 * a plaintext access token is allowed to materialize.
 */
export interface PlaidTokenBroker {
  withDecryptedToken<T>(input: {
    readonly itemId: ItemId;
    readonly use: (token: PlaidAccessTokenInMemory) => Promise<T>;
  }): Promise<Result<T, PlaidError | CryptoError>>;
}

// -----------------------------------------------------------------------------
// AuditService
// -----------------------------------------------------------------------------
// Implementation in `lib/audit/`. Append-only, hash-chained.
// =============================================================================

export interface AuditAppendInput {
  readonly actorUserId: UserId | null;
  readonly actorKind: ActorKind;
  readonly domain: Domain | null;
  readonly subjectId: string | null;
  readonly subjectKind: SubjectKind | null;
  readonly action: AuditAction;
  readonly outcome: AuditOutcome;
  /** Sanitized before persistence. MUST NOT contain tokens / keys / secrets. */
  readonly details: Readonly<Record<string, unknown>>;
}

export interface AuditQueryInput {
  readonly fromSeq?: AuditSeq;
  readonly toSeq?: AuditSeq;
  readonly fromTs?: Date;
  readonly toTs?: Date;
  readonly actorUserId?: UserId;
  readonly action?: AuditAction;
  readonly domain?: Domain;
  readonly limit?: number;
}

export interface AuditService {
  append(input: AuditAppendInput): Promise<Result<AuditEntry, AuditError>>;
  query(input: AuditQueryInput): Promise<Result<ReadonlyArray<AuditEntry>, AuditError>>;
  /** Walk the chain seq-ascending, recompute every hash. */
  verifyChain(): Promise<Result<{ readonly verifiedCount: number }, AuditError>>;
}

// -----------------------------------------------------------------------------
// Repository<T> — generic shape
// -----------------------------------------------------------------------------
// Repository implementations live in `lib/db/`. They are the ONLY place that
// holds a Prisma client. Route handlers / services depend on these interfaces.
//
// Scope-by-construction: every repo factory takes a "scope" (UserId for
// personal, "pcc-membership-of:<UserId>" for PCC). The scope is applied as a
// WHERE clause on every read and an assertion on every write. Code that
// doesn't have a scope physically cannot read another user's rows.
// =============================================================================

export type RepoScope =
  | { readonly kind: 'personal'; readonly userId: UserId }
  | { readonly kind: 'pcc'; readonly memberOfUserId: UserId }
  | { readonly kind: 'admin' }; // admin CLI only — never injected into route handlers

export interface UserRepository {
  findByEmail(email: string): Promise<Result<User | null, RepoError>>;
  findById(id: UserId): Promise<Result<User | null, RepoError>>;
  list(): Promise<Result<ReadonlyArray<User>, RepoError>>;
  create(input: {
    readonly email: string;
    readonly displayName: string;
    readonly role: Role;
  }): Promise<Result<User, RepoError>>;
  setWrappedUserDek(input: {
    readonly userId: UserId;
    readonly version: number;
    readonly wrapped: EncryptedBlob;
  }): Promise<Result<void, RepoError>>;
}

export interface PasskeyRepository {
  findByCredentialId(credentialId: Uint8Array): Promise<Result<Passkey | null, RepoError>>;
  listByUser(userId: UserId): Promise<Result<ReadonlyArray<Passkey>, RepoError>>;
  create(input: {
    readonly userId: UserId;
    readonly credentialId: Uint8Array;
    readonly credentialPublicKey: Uint8Array;
    readonly counter: bigint;
    readonly transports: ReadonlyArray<string>;
    readonly aaguid: Uint8Array | null;
    readonly deviceLabel: string | null;
    readonly kekSalt: Uint8Array;
  }): Promise<Result<Passkey, RepoError>>;
  bumpCounter(input: { readonly id: PasskeyId; readonly newCounter: bigint }): Promise<Result<void, RepoError>>;
  revoke(input: { readonly id: PasskeyId }): Promise<Result<void, RepoError>>;
}

export interface SessionRepository {
  create(input: {
    readonly userId: UserId;
    readonly expiresAt: Date;
    readonly idleTimeoutAt: Date;
    readonly userAgent: string | null;
    readonly remoteAddr: string | null;
  }): Promise<Result<Session, RepoError>>;
  findActiveById(id: SessionId): Promise<Result<Session | null, RepoError>>;
  findActiveByUser(userId: UserId): Promise<Result<Session | null, RepoError>>;
  touch(input: { readonly id: SessionId; readonly newIdleTimeoutAt: Date }): Promise<Result<void, RepoError>>;
  revoke(input: { readonly id: SessionId; readonly reason: string }): Promise<Result<void, RepoError>>;
  revokeAllActive(input: { readonly reason: string }): Promise<Result<{ readonly count: number }, RepoError>>;
  expireOverdue(now: Date): Promise<Result<{ readonly count: number }, RepoError>>;
}

export interface ItemRepository {
  /** Personal scope: returns only items where item.userId === scope.userId.
   *  PCC scope: returns only items where item.domain === 'pcc' AND scope's
   *  user has an active PccMembership. Admin scope: all rows. */
  list(scope: RepoScope, filter?: { readonly domain?: Domain }): Promise<Result<ReadonlyArray<Item>, RepoError>>;
  findById(scope: RepoScope, id: ItemId): Promise<Result<Item | null, RepoError>>;
  create(scope: RepoScope, input: {
    readonly domain: Domain;
    readonly userId: UserId | null;
    readonly plaidItemId: PlaidItemId;
    readonly plaidInstitutionId: string | null;
    readonly institutionName: string | null;
    readonly encryptedAccessToken: EncryptedBlob;
  }): Promise<Result<Item, RepoError>>;
  /** Read the encrypted access_token blob. Visibility rules same as `findById`. */
  readEncryptedToken(scope: RepoScope, id: ItemId): Promise<Result<EncryptedBlob, RepoError>>;
  /** Used by master-rotation. Admin scope only. */
  rewriteEncryptedToken(scope: RepoScope, input: {
    readonly id: ItemId;
    readonly newBlob: EncryptedBlob;
  }): Promise<Result<void, RepoError>>;
  updateSyncCursor(scope: RepoScope, input: {
    readonly id: ItemId;
    readonly cursor: string;
    readonly outcome: 'success' | 'error' | 'pending';
  }): Promise<Result<void, RepoError>>;
  softRemove(scope: RepoScope, input: { readonly id: ItemId; readonly reason: string }): Promise<Result<void, RepoError>>;
}

export interface AccountRepository {
  listByItem(scope: RepoScope, itemId: ItemId): Promise<Result<ReadonlyArray<Account>, RepoError>>;
  upsertFromPlaid(scope: RepoScope, input: {
    readonly itemId: ItemId;
    readonly accounts: ReadonlyArray<{
      readonly plaidAccountId: PlaidAccountId;
      readonly name: string;
      readonly officialName: string | null;
      readonly mask: string | null;
      readonly type: Account['type'];
      readonly subtype: string | null;
      readonly isoCurrencyCode: IsoCurrencyCode;
      readonly currentBalanceCents: Cents | null;
      readonly availableBalanceCents: Cents | null;
      readonly limitCents: Cents | null;
    }>;
  }): Promise<Result<{ readonly upserted: number }, RepoError>>;
  listAllInScope(scope: RepoScope, filter?: { readonly domain?: Domain }): Promise<Result<ReadonlyArray<Account>, RepoError>>;
}

export interface TransactionRepository {
  applyPlaidSync(scope: RepoScope, input: {
    readonly itemId: ItemId;
    readonly added: ReadonlyArray<TransactionInput>;
    readonly modified: ReadonlyArray<TransactionInput>;
    readonly removedPlaidIds: ReadonlyArray<PlaidTransactionId>;
  }): Promise<Result<{ readonly added: number; readonly modified: number; readonly removed: number }, RepoError>>;
  listByDateRange(scope: RepoScope, input: {
    readonly fromDate: Date;
    readonly toDate: Date;
    readonly domain?: Domain;
  }): Promise<Result<ReadonlyArray<Transaction>, RepoError>>;
}

export interface TransactionInput {
  readonly itemId: ItemId;
  readonly accountId: AccountId;
  readonly domain: Domain;
  readonly userId: UserId | null;
  readonly plaidTransactionId: PlaidTransactionId;
  readonly amountCents: Cents;
  readonly isoCurrencyCode: IsoCurrencyCode;
  readonly date: Date;
  readonly authorizedDate: Date | null;
  readonly name: string;
  readonly merchantName: string | null;
  readonly pending: boolean;
  readonly category: string | null;
  readonly categoryDetailed: string | null;
}

export interface SnapshotRepository {
  insert(scope: RepoScope, input: Omit<NetWorthSnapshot, 'id'>): Promise<Result<NetWorthSnapshot, RepoError>>;
  latest(scope: RepoScope, input: { readonly domain: Domain; readonly userId: UserId | null }): Promise<Result<NetWorthSnapshot | null, RepoError>>;
  series(scope: RepoScope, input: {
    readonly domain: Domain;
    readonly userId: UserId | null;
    readonly fromTs: Date;
    readonly toTs: Date;
  }): Promise<Result<ReadonlyArray<NetWorthSnapshot>, RepoError>>;
}

export interface PccMembershipRepository {
  isActiveMember(userId: UserId): Promise<Result<boolean, RepoError>>;
  list(): Promise<Result<ReadonlyArray<PccMembership>, RepoError>>;
  add(input: { readonly userId: UserId }): Promise<Result<PccMembership, RepoError>>;
  revoke(input: { readonly userId: UserId }): Promise<Result<void, RepoError>>;
}

// -----------------------------------------------------------------------------
// IPC keybridge — shared between web process and sync worker
// -----------------------------------------------------------------------------
// Web process listens on /tmp/greylock-keybridge.sock (mode 0600). Sync worker
// connects, presents its peer credentials (verified via macOS LOCAL_PEERCRED),
// then issues authenticated `requestDek` calls. The web process returns a
// short-lived DEK reference that lives only inside the sync worker's address
// space for the duration of the sync run.
//
// Wire format (newline-delimited JSON, length-prefixed Bytes for keys):
//   line 1: HELLO     {"v":1, "pid":<int>, "uid":<int>}
//   line 2: AUTH      {"nonce":<base64>, "hmac":<base64>}  // server replies OK or DENY
//   line 3..: REQUEST {"id":<uuid>, "method":"requestDek", "params":{"userId":<UserId>, "sessionId":<SessionId>}}
//             RESPONSE {"id":<uuid>, "ok":true|false, ... }
//
// Detailed protocol: docs/API.md §Keybridge.
// =============================================================================

export interface KeybridgeServer {
  /** Bind socket, set mode 0600, start accepting peers. Idempotent. */
  start(): Promise<Result<void, KeybridgeError>>;
  /** Close socket; gracefully refuse pending in-flight requests. */
  stop(): Promise<void>;
  /** Hooked by AuthService when a session is revoked → tells worker the
   *  matching DEK is no longer available so its next call fails fast. */
  invalidateSession(sessionId: SessionId): Promise<void>;
}

export interface KeybridgeClient {
  connect(): Promise<Result<void, KeybridgeError>>;
  disconnect(): Promise<void>;
  /** Returned key handle is valid only inside the sync worker's process.
   *  The actual key bytes are sent over the socket and zeroized when the
   *  worker calls `releaseDek`. */
  requestDek(input: { readonly userId: UserId; readonly sessionId: SessionId }): Promise<Result<KeyHandle, KeybridgeError>>;
  releaseDek(handle: KeyHandle): Promise<void>;
}

export type KeybridgeError =
  | { readonly kind: 'socket_unavailable' }
  | { readonly kind: 'peer_credential_mismatch' }
  | { readonly kind: 'auth_failed' }
  | { readonly kind: 'session_invalid' }
  | { readonly kind: 'dek_unavailable' }
  | { readonly kind: 'protocol_error' }
  | { readonly kind: 'timeout' };

// -----------------------------------------------------------------------------
// Compute service — pure functions
// -----------------------------------------------------------------------------
// Implementation in `lib/compute/`. No I/O, no Date.now() inside formulas
// (callers pass `now`), fully unit-testable from fixtures.
// =============================================================================

export interface ComputeService {
  netWorth(input: { readonly accounts: ReadonlyArray<Account> }): NetWorthResult;
  cashOnly(input: { readonly accounts: ReadonlyArray<Account> }): Cents;
  monthNet(input: {
    readonly transactions: ReadonlyArray<Transaction>;
    readonly now: Date;
  }): MonthNetResult;
  billionProgress(input: { readonly netWorthCents: Cents }): BillionProgressResult;
}

// -----------------------------------------------------------------------------
// Sync orchestrator
// -----------------------------------------------------------------------------

export interface SyncOrchestrator {
  /** One pass: PCC items always, personal items for users with active sessions. */
  runOnce(input: { readonly now: Date }): Promise<Result<SyncOutcome, SyncError>>;
  /** Manual sync of a single item from the UI. Caller must already hold the
   *  appropriate session. */
  syncItem(input: { readonly itemId: ItemId; readonly initiatorUserId: UserId }): Promise<Result<PlaidSyncResult, SyncError>>;
}

export interface SyncOutcome {
  readonly itemsAttempted: number;
  readonly itemsSucceeded: number;
  readonly itemsFailed: number;
  readonly snapshotsWritten: number;
  readonly durationMs: number;
}

export type SyncError =
  | { readonly kind: 'crypto_unavailable' }
  | { readonly kind: 'keybridge_unavailable' }
  | { readonly kind: 'partial_failure'; readonly failures: number }
  | { readonly kind: 'unexpected'; readonly cause: string };

// Re-export the PlaidSyncResult symbol for convenience callers.
export type { PlaidSyncResult as SyncItemResult };
