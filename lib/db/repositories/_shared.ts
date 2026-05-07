// Greylock — repository shared helpers
// =============================================================================
// AGENT-DB. Internal helpers shared across every repository file. NEVER
// imported outside `lib/db/`.
//
// Hard rules these helpers enforce:
//   - All repository methods return `Result<T, RepoError>`. We catch every
//     Prisma error here and map to a `RepoError` discriminant. Out-of-scope
//     reads return `not_found` (NOT `unauthorized`) — see SPEC §7 / Threat
//     Model §1.2.1: distinguishable error types let an attacker enumerate
//     row IDs.
//   - `RepoScope` is applied unconditionally inside the WHERE clause builder.
//     Admin scope means "no additional scope filter", but it ALWAYS goes
//     through the same query builder — never through a separate "if admin"
//     branch with raw findMany.
// =============================================================================

import type { Prisma, PrismaClient } from '@prisma/client';

import { Err, Ok } from '../../types/domain.js';
import type { Domain, RepoError, Result, UserId } from '../../types/domain.js';
import type { RepoScope } from '../../types/services.js';

/** Single shared symbol for our typed Prisma client and any transactional
 *  client (the type Prisma exposes inside `$transaction(async (tx) => ...)`).
 *  Using this consistently means a repository method can be passed a tx
 *  client without changing its signature. */
export type TxClient = PrismaClient | Prisma.TransactionClient;

// -----------------------------------------------------------------------------
// Error mapping
// -----------------------------------------------------------------------------

export function mapPrismaError(err: unknown): RepoError {
  // We deliberately do NOT include error message text in the RepoError so a
  // caller can't accidentally surface DB internals over the API. Logs go
  // through audit only.
  if (err === null || typeof err !== 'object') {
    return { kind: 'storage_failure' };
  }
  const obj = err as { code?: unknown; meta?: unknown };
  if (obj.code === 'P2002') {
    return { kind: 'conflict' };
  }
  if (obj.code === 'P2025') {
    return { kind: 'not_found' };
  }
  return { kind: 'storage_failure' };
}

/** Run a Prisma operation and convert thrown errors to RepoError. */
export async function tryDb<T>(op: () => Promise<T>): Promise<Result<T, RepoError>> {
  try {
    const v = await op();
    return Ok(v);
  } catch (cause: unknown) {
    return Err(mapPrismaError(cause));
  }
}

// -----------------------------------------------------------------------------
// Scope-by-construction helpers
// -----------------------------------------------------------------------------

/**
 * Build the WHERE-fragment for queries on rows that have a `userId` (personal
 * domain) and may also be visible via PCC membership.
 *
 * - personal scope: WHERE userId = scope.userId (rows with userId IS NULL never
 *                   leak into a personal scope).
 * - pcc scope:      WHERE domain = 'pcc' AND scope's user has an active
 *                   PccMembership. The membership predicate is encoded as a
 *                   correlated subquery against the PccMembership table —
 *                   inlined into the outer where clause so the database's
 *                   query planner handles row visibility, not application
 *                   code.
 * - admin scope:    no extra filter (the query builder still uses this fn,
 *                   just returns an empty fragment).
 *
 * The returned fragment is a Prisma `WhereInput` (typed `unknown` to allow
 * the caller to spread it into model-specific where types). Each repository
 * narrows it to its model's WhereInput.
 */
export type ScopeFilter<W> = W & Record<string, unknown>;

export interface ItemScopeOptions {
  /** Optional override: filter to a specific domain (`personal` or `pcc`). */
  readonly domain?: Domain;
}

/**
 * Build the WHERE filter applied to the `Item` model when the caller has
 * already verified PCC membership (or scope is personal/admin). For PCC
 * scope the membership check itself MUST be done by the caller via
 * `requirePccMembership()` BEFORE any read; this function only filters by
 * domain. That keeps the SQL planner happy (no cross-table EXISTS in a
 * Prisma relation filter) and surfaces "non-member" as `not_found` cleanly.
 *
 * Used as `where: { ...itemScopeFilter(scope) }` directly on Item, or
 * spread into `where: { item: { is: itemScopeFilter(scope) } }` for child
 * tables (Account, Transaction).
 */
export function itemScopeFilter(
  scope: RepoScope,
  opts: ItemScopeOptions = {},
): Prisma.ItemWhereInput {
  const base: Prisma.ItemWhereInput = {};
  if (opts.domain !== undefined) {
    base.domain = opts.domain;
  }
  switch (scope.kind) {
    case 'personal':
      return { ...base, domain: 'personal', userId: scope.userId };
    case 'pcc':
      return { ...base, domain: 'pcc' };
    case 'admin':
      return base;
  }
}

/**
 * Predicate for "scope's user is an active PCC member". Returns the userId
 * to membership-check, or null if the scope is not PCC.
 */
export function pccMembershipPredicateUserId(scope: RepoScope): UserId | null {
  return scope.kind === 'pcc' ? scope.memberOfUserId : null;
}

/**
 * Asserts the caller's scope is allowed to act on PCC rows. For PCC scope
 * looks up `PccMembership` and checks `revokedAt IS NULL`. For personal and
 * admin scopes returns `Ok(true)` immediately.
 *
 * Returns `Err({kind:'not_found'})` if the PCC scope's user is NOT an active
 * member — deliberately indistinguishable from "row doesn't exist" so
 * non-members can't enumerate PCC item IDs by error type (SPEC §7).
 */
export async function requirePccMembershipOrNotFound(args: {
  readonly scope: RepoScope;
  readonly prisma: TxClient;
}): Promise<Result<void, RepoError>> {
  if (args.scope.kind !== 'pcc') {
    return Ok(undefined);
  }
  const userId = args.scope.memberOfUserId;
  try {
    const row = await args.prisma.pccMembership.findFirst({
      where: { userId, revokedAt: null },
      select: { id: true },
    });
    if (row === null) {
      return Err({ kind: 'not_found' });
    }
    return Ok(undefined);
  } catch (cause: unknown) {
    return Err(mapPrismaError(cause));
  }
}

/**
 * Returns true if the given scope can see the given item. Used as a guard
 * for write methods (where we already loaded the row by id and want to make
 * sure the caller is allowed to mutate it).
 */
export interface ItemRowForScopeCheck {
  readonly domain: Domain;
  readonly userId: string | null;
}

export async function isItemVisibleToScope(args: {
  readonly scope: RepoScope;
  readonly item: ItemRowForScopeCheck;
  /** Async checker for PCC active membership. Receives the scope's user id. */
  readonly isActivePccMember: (userId: UserId) => Promise<boolean>;
}): Promise<boolean> {
  switch (args.scope.kind) {
    case 'admin':
      return true;
    case 'personal':
      return args.item.domain === 'personal' && args.item.userId === args.scope.userId;
    case 'pcc': {
      if (args.item.domain !== 'pcc') {
        return false;
      }
      return args.isActivePccMember(args.scope.memberOfUserId);
    }
  }
}

// -----------------------------------------------------------------------------
// Bytes <-> Buffer helpers
// -----------------------------------------------------------------------------

/** Prisma SQLite gives us `Buffer` for `Bytes` columns; domain types use
 *  `Uint8Array`. The two are interchangeable for callers but we normalize so
 *  consumers don't need to discriminate. */
export function asBytes(b: Buffer | Uint8Array): Uint8Array {
  if (b instanceof Uint8Array && !(b instanceof Buffer)) {
    return b;
  }
  // Buffer extends Uint8Array; copy to a fresh Uint8Array so callers can't
  // accidentally retain a Prisma-cached Buffer reference.
  return new Uint8Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
}

/** Convert input Uint8Array to a fresh ArrayBuffer-backed `Uint8Array<ArrayBuffer>`
 *  for Prisma writes. Prisma's `Bytes` field accepts exactly that variance
 *  (not `Uint8Array<ArrayBufferLike>`), so we allocate a new `ArrayBuffer`
 *  here, copy in the source bytes, and return the typed view. The fresh
 *  ArrayBuffer also means the returned bytes don't alias caller memory. */
export function asBuffer(b: Uint8Array): Uint8Array<ArrayBuffer> {
  const ab = new ArrayBuffer(b.byteLength);
  const view = new Uint8Array(ab);
  view.set(b);
  return view;
}

// -----------------------------------------------------------------------------
// Branded-string casts for Plaid id types
// -----------------------------------------------------------------------------
//
// `lib/types/domain.ts` only ships smart constructors for the Greylock-side
// IDs (UserId, ItemId, …). The Plaid-side branded types (PlaidItemId,
// PlaidAccountId, PlaidTransactionId) are types-only. AGENT-DB does not edit
// `lib/types/*`, so we keep these tiny local cast helpers so the `as` cast
// is colocated with the repository boundary rather than scattered through
// every `toDomain()` mapper.
// -----------------------------------------------------------------------------

import type { PlaidAccountId, PlaidItemId, PlaidTransactionId } from '../../types/domain.js';

export const asPlaidItemId = (s: string): PlaidItemId => s as PlaidItemId;
export const asPlaidAccountId = (s: string): PlaidAccountId => s as PlaidAccountId;
export const asPlaidTransactionId = (s: string): PlaidTransactionId => s as PlaidTransactionId;
