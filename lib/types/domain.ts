// Greylock — domain types
// =============================================================================
// AGENT-ARCH (Phase 1). Pure domain types decoupled from Prisma generated
// types. The repository layer (lib/db) returns these; route handlers and
// services consume these. No Prisma imports here on purpose — this file must
// compile without `node_modules/.prisma/*` existing.
//
// All monetary values are MINOR UNITS (cents) as `bigint`. USD only for v0.1.
// =============================================================================

// -----------------------------------------------------------------------------
// Brands — nominal types so we can't accidentally pass a UserId as an ItemId.
// -----------------------------------------------------------------------------

declare const Brand: unique symbol;
type Branded<T, B extends string> = T & { readonly [Brand]: B };

export type UserId = Branded<string, 'UserId'>;
export type SessionId = Branded<string, 'SessionId'>;
export type ItemId = Branded<string, 'ItemId'>;
export type AccountId = Branded<string, 'AccountId'>;
export type TransactionId = Branded<string, 'TransactionId'>;
export type SnapshotId = Branded<string, 'SnapshotId'>;
export type PasskeyId = Branded<string, 'PasskeyId'>;
export type AuditSeq = Branded<bigint, 'AuditSeq'>;

export type PlaidItemId = Branded<string, 'PlaidItemId'>;
export type PlaidAccountId = Branded<string, 'PlaidAccountId'>;
export type PlaidTransactionId = Branded<string, 'PlaidTransactionId'>;
export type PlaidLinkToken = Branded<string, 'PlaidLinkToken'>;
export type PlaidPublicToken = Branded<string, 'PlaidPublicToken'>;

/** Branded marker for a Plaid access token loaded into memory after decryption. */
export type PlaidAccessTokenInMemory = Branded<string, 'PlaidAccessTokenInMemory'>;

// Smart constructors — repos / services use these. Only place `as` casts may
// appear in the codebase, and only behind these helpers.
export const UserId = (s: string): UserId => s as UserId;
export const SessionId = (s: string): SessionId => s as SessionId;
export const ItemId = (s: string): ItemId => s as ItemId;
export const AccountId = (s: string): AccountId => s as AccountId;
export const TransactionId = (s: string): TransactionId => s as TransactionId;
export const SnapshotId = (s: string): SnapshotId => s as SnapshotId;
export const PasskeyId = (s: string): PasskeyId => s as PasskeyId;
export const AuditSeq = (n: bigint): AuditSeq => n as AuditSeq;

// -----------------------------------------------------------------------------
// Enums — mirror Prisma but defined here so this module is self-contained.
// -----------------------------------------------------------------------------

export const Domain = {
  Personal: 'personal',
  Pcc: 'pcc',
} as const;
export type Domain = (typeof Domain)[keyof typeof Domain];

export const Role = {
  Owner: 'owner',
  Member: 'member',
} as const;
export type Role = (typeof Role)[keyof typeof Role];

export const SessionStatus = {
  Active: 'active',
  Revoked: 'revoked',
  Expired: 'expired',
} as const;
export type SessionStatus = (typeof SessionStatus)[keyof typeof SessionStatus];

export const AuditAction = {
  PasskeyEnrollment: 'passkey_enrollment',
  PasskeyEnrollmentRejected: 'passkey_enrollment_rejected',
  PasskeyAuthenticationSuccess: 'passkey_authentication_success',
  PasskeyAuthenticationFailure: 'passkey_authentication_failure',
  SessionCreated: 'session_created',
  SessionRevoked: 'session_revoked',
  SessionExpired: 'session_expired',
  PlaidLinkTokenMinted: 'plaid_link_token_minted',
  PlaidPublicTokenExchanged: 'plaid_public_token_exchanged',
  PlaidItemAdded: 'plaid_item_added',
  PlaidItemRemoved: 'plaid_item_removed',
  PlaidTokenDecrypted: 'plaid_token_decrypted',
  PlaidSyncStarted: 'plaid_sync_started',
  PlaidSyncCompleted: 'plaid_sync_completed',
  PlaidSyncFailed: 'plaid_sync_failed',
  NetWorthSnapshotWritten: 'net_worth_snapshot_written',
  AdminEnrollInvoked: 'admin_enroll_invoked',
  AdminRevokeInvoked: 'admin_revoke_invoked',
  AdminRevokeAllInvoked: 'admin_revoke_all_invoked',
  AdminMasterRotationStarted: 'admin_master_rotation_started',
  AdminMasterRotationCompleted: 'admin_master_rotation_completed',
  AdminMasterRotationFailed: 'admin_master_rotation_failed',
  AdminAuditVerifyInvoked: 'admin_audit_verify_invoked',
  MasterKekLoaded: 'master_kek_loaded',
  MasterKekUnloaded: 'master_kek_unloaded',
  PccDekUnwrapped: 'pcc_dek_unwrapped',
  PccDekZeroized: 'pcc_dek_zeroized',
  PerUserDekDerived: 'per_user_dek_derived',
  PerUserDekZeroized: 'per_user_dek_zeroized',
  RateLimitTripped: 'rate_limit_tripped',
  IpcKeybridgeRequestDenied: 'ipc_keybridge_request_denied',
} as const;
export type AuditAction = (typeof AuditAction)[keyof typeof AuditAction];

export const AuditOutcome = {
  Success: 'success',
  Failure: 'failure',
  Denied: 'denied',
} as const;
export type AuditOutcome = (typeof AuditOutcome)[keyof typeof AuditOutcome];

export const ActorKind = {
  User: 'user',
  System: 'system',
  AdminCli: 'admin_cli',
  SyncWorker: 'sync_worker',
} as const;
export type ActorKind = (typeof ActorKind)[keyof typeof ActorKind];

export const SubjectKind = {
  Session: 'session',
  Item: 'item',
  Snapshot: 'snapshot',
  Passkey: 'passkey',
  User: 'user',
} as const;
export type SubjectKind = (typeof SubjectKind)[keyof typeof SubjectKind];

// -----------------------------------------------------------------------------
// Currency
// -----------------------------------------------------------------------------

/** All money values are MINOR UNITS (cents). bigint to avoid float drift. */
export type Cents = bigint;
/** ISO-4217 alpha. v0.1 only "USD". */
export type IsoCurrencyCode = 'USD';

// -----------------------------------------------------------------------------
// Encrypted blob shape
// -----------------------------------------------------------------------------

/**
 * Layout: version(1) || domain_tag(1) || nonce(12) || ciphertext(N) || tag(16)
 * AAD is bound to the row identity and domain (see ARCHITECTURE.md §AAD).
 */
export type EncryptedBlob = Branded<Uint8Array, 'EncryptedBlob'>;

export const EncryptedBlob = {
  /** Construct from already-validated bytes. NEVER call from non-crypto code. */
  unsafeFromBytes: (b: Uint8Array): EncryptedBlob => b as EncryptedBlob,
};

export const DomainTag = {
  Personal: 0x01,
  Pcc: 0x02,
} as const;
export type DomainTag = (typeof DomainTag)[keyof typeof DomainTag];

// -----------------------------------------------------------------------------
// Domain entities
// -----------------------------------------------------------------------------

export interface User {
  readonly id: UserId;
  readonly email: string;
  readonly displayName: string;
  readonly role: Role;
  readonly userDekVersion: number;
  readonly createdAt: Date;
  readonly updatedAt: Date;
}

export interface Passkey {
  readonly id: PasskeyId;
  readonly userId: UserId;
  readonly credentialId: Uint8Array;
  readonly credentialPublicKey: Uint8Array;
  readonly counter: bigint;
  readonly transports: ReadonlyArray<string>;
  readonly aaguid: Uint8Array | null;
  readonly deviceLabel: string | null;
  readonly createdAt: Date;
  readonly lastUsedAt: Date | null;
  readonly revokedAt: Date | null;
}

export interface Session {
  readonly id: SessionId;
  readonly userId: UserId;
  readonly status: SessionStatus;
  readonly createdAt: Date;
  readonly lastActivityAt: Date;
  readonly expiresAt: Date;
  readonly idleTimeoutAt: Date;
  readonly revokedAt: Date | null;
  readonly revokedReason: string | null;
  readonly userAgent: string | null;
  readonly remoteAddr: string | null;
}

export interface PccMembership {
  readonly userId: UserId;
  readonly joinedAt: Date;
  readonly revokedAt: Date | null;
}

export interface Item {
  readonly id: ItemId;
  readonly domain: Domain;
  readonly userId: UserId | null;
  readonly plaidItemId: PlaidItemId;
  readonly plaidInstitutionId: string | null;
  readonly institutionName: string | null;
  readonly syncCursor: string | null;
  readonly lastSyncAt: Date | null;
  readonly lastSyncOutcome: 'success' | 'error' | 'pending' | null;
  readonly consecutiveFailures: number;
  readonly createdAt: Date;
  readonly updatedAt: Date;
  readonly removedAt: Date | null;
  readonly removedReason: string | null;
}

export interface Account {
  readonly id: AccountId;
  readonly itemId: ItemId;
  readonly domain: Domain;
  readonly userId: UserId | null;
  readonly plaidAccountId: PlaidAccountId;
  readonly name: string;
  readonly officialName: string | null;
  readonly mask: string | null;
  readonly type: 'depository' | 'credit' | 'loan' | 'investment' | 'other';
  readonly subtype: string | null;
  readonly isoCurrencyCode: IsoCurrencyCode;
  readonly currentBalanceCents: Cents | null;
  readonly availableBalanceCents: Cents | null;
  readonly limitCents: Cents | null;
  readonly balanceUpdatedAt: Date | null;
  readonly createdAt: Date;
  readonly updatedAt: Date;
  readonly closedAt: Date | null;
}

export interface Transaction {
  readonly id: TransactionId;
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
  readonly createdAt: Date;
  readonly updatedAt: Date;
  readonly removedAt: Date | null;
}

export interface NetWorthSnapshot {
  readonly id: SnapshotId;
  readonly domain: Domain;
  readonly userId: UserId | null;
  readonly takenAt: Date;
  readonly assetsCents: Cents;
  readonly liabilitiesCents: Cents;
  readonly netWorthCents: Cents;
  readonly cashCents: Cents;
  readonly monthNetCents: Cents | null;
  readonly computeVersion: number;
  readonly breakdownJson: string;
}

export interface AuditEntry {
  readonly seq: AuditSeq;
  readonly ts: Date;
  readonly tsNanos: number;
  readonly actorUserId: UserId | null;
  readonly actorKind: ActorKind;
  readonly domain: Domain | null;
  readonly subjectId: string | null;
  readonly subjectKind: SubjectKind | null;
  readonly action: AuditAction;
  readonly outcome: AuditOutcome;
  readonly detailsJson: string;
  readonly prevHash: Uint8Array;
  readonly entryHash: Uint8Array;
}

// -----------------------------------------------------------------------------
// Compute layer outputs (pure functions; no side effects)
// -----------------------------------------------------------------------------

export interface NetWorthBreakdownLine {
  readonly accountId: AccountId;
  readonly name: string;
  readonly type: Account['type'];
  readonly balanceCents: Cents;
  readonly contribution: 'asset' | 'liability';
}

export interface NetWorthResult {
  readonly assetsCents: Cents;
  readonly liabilitiesCents: Cents;
  readonly netWorthCents: Cents;
  readonly cashCents: Cents;
  readonly breakdown: ReadonlyArray<NetWorthBreakdownLine>;
}

export interface MonthNetResult {
  /** Inclusive start of the 30-day window. */
  readonly windowStart: Date;
  /** Exclusive end. */
  readonly windowEnd: Date;
  readonly inflowCents: Cents;
  readonly outflowCents: Cents;
  readonly netCents: Cents;
}

export interface BillionProgressResult {
  /** Net worth in cents. */
  readonly netWorthCents: Cents;
  /** Goal in cents (1_000_000_000_00). */
  readonly goalCents: Cents;
  /** netWorth / goal, clamped to [0, 1]. */
  readonly progress: number;
}

// -----------------------------------------------------------------------------
// Result type — used by services so errors are values, not exceptions.
// Auth and crypto failures must NOT be thrown across module boundaries; they
// are returned as `Err` so callers cannot accidentally swallow them.
// -----------------------------------------------------------------------------

export type Result<T, E> = { readonly ok: true; readonly value: T } | { readonly ok: false; readonly error: E };

export const Ok = <T,>(value: T): Result<T, never> => ({ ok: true, value });
export const Err = <E,>(error: E): Result<never, E> => ({ ok: false, error });

// -----------------------------------------------------------------------------
// Service errors
// -----------------------------------------------------------------------------

export type CryptoError =
  | { readonly kind: 'master_passphrase_unavailable' }
  | { readonly kind: 'pcc_dek_not_loaded' }
  | { readonly kind: 'user_dek_not_loaded'; readonly userId: UserId }
  | { readonly kind: 'aad_mismatch' }
  | { readonly kind: 'tag_invalid' }
  | { readonly kind: 'malformed_blob' }
  | { readonly kind: 'kdf_failure' }
  | { readonly kind: 'rotation_in_progress' };

export type AuthError =
  | { readonly kind: 'email_not_allowlisted' }
  | { readonly kind: 'placeholder_email_rejected' }
  | { readonly kind: 'no_passkey_for_email' }
  | { readonly kind: 'passkey_already_enrolled' }
  | { readonly kind: 'webauthn_verification_failed' }
  | { readonly kind: 'session_expired' }
  | { readonly kind: 'session_revoked' }
  | { readonly kind: 'session_not_found' }
  | { readonly kind: 'rate_limited'; readonly retryAfterSeconds: number };

export type PlaidError =
  | { readonly kind: 'invalid_public_token' }
  | { readonly kind: 'plaid_api_error'; readonly httpStatus: number; readonly errorCode: string }
  | { readonly kind: 'item_not_found' }
  | { readonly kind: 'token_decrypt_failed' };

export type AuditError =
  | { readonly kind: 'chain_break'; readonly atSeq: AuditSeq }
  | { readonly kind: 'sanitizer_rejected_payload' }
  | { readonly kind: 'storage_failure' };

export type RepoError =
  | { readonly kind: 'not_found' }
  | { readonly kind: 'conflict' }
  | { readonly kind: 'unauthorized' }
  | { readonly kind: 'storage_failure' };
