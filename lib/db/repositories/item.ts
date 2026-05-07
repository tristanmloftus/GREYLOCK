// Greylock — ItemRepository (scope-by-construction)
// =============================================================================
// AGENT-DB. Implementation of the most security-critical repository: every
// read and every write is gated on `RepoScope`.
//
// Visibility rules (SPEC §7 / Threat Model §1.2.1 / docs/ARCHITECTURE.md §3):
//   - personal: WHERE domain='personal' AND userId = scope.userId
//   - pcc:      caller MUST be an active PccMembership; then WHERE domain='pcc'
//   - admin:    no extra filter
//
// Out-of-scope reads return `Err({kind:'not_found'})`. Never `unauthorized`.
//
// `Item.encryptedAccessToken` is `Bytes`. We never log it, never include it
// in error metadata, never let it appear in a query string.
// =============================================================================

import type { PrismaClient, Item as PrismaItem, Prisma } from '@prisma/client';

import { Err, Ok, ItemId, UserId } from '../../types/domain.js';
import type { Domain, EncryptedBlob, Item, RepoError, Result } from '../../types/domain.js';
import type { ItemRepository, RepoScope } from '../../types/services.js';

import {
  asBuffer,
  asBytes,
  asPlaidItemId,
  itemScopeFilter,
  mapPrismaError,
  requirePccMembershipOrNotFound,
  tryDb,
} from './_shared.js';

function toDomain(row: PrismaItem): Item {
  return {
    id: ItemId(row.id),
    domain: row.domain as Domain,
    userId: row.userId === null ? null : UserId(row.userId),
    plaidItemId: asPlaidItemId(row.plaidItemId),
    plaidInstitutionId: row.plaidInstitutionId,
    institutionName: row.institutionName,
    syncCursor: row.syncCursor,
    lastSyncAt: row.lastSyncAt,
    lastSyncOutcome:
      row.lastSyncOutcome === null
        ? null
        : (row.lastSyncOutcome as 'success' | 'error' | 'pending'),
    consecutiveFailures: row.consecutiveFailures,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
    removedAt: row.removedAt,
    removedReason: row.removedReason,
  };
}

export interface CreateItemRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createItemRepository(input: CreateItemRepositoryInput): ItemRepository {
  const { prisma } = input;

  /**
   * Single source of truth for "build the where for this scope". Every read
   * AND write goes through this function. There is no "if admin then no
   * filter" early return that bypasses scope evaluation.
   */
  function whereForScope(
    scope: RepoScope,
    extra: Prisma.ItemWhereInput = {},
  ): Prisma.ItemWhereInput {
    return { ...itemScopeFilter(scope), ...extra };
  }

  return {
    async list(scope, filter): Promise<Result<ReadonlyArray<Item>, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const where = whereForScope(
          scope,
          filter?.domain !== undefined ? { domain: filter.domain } : {},
        );
        const rows = await prisma.item.findMany({
          where,
          orderBy: { createdAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },

    async findById(scope, id): Promise<Result<Item | null, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const row = await prisma.item.findFirst({
          where: whereForScope(scope, { id }),
        });
        return row === null ? null : toDomain(row);
      });
    },

    async create(scope, data): Promise<Result<Item, RepoError>> {
      // Scope must consent to writing rows of this domain.
      const consent = consentToWrite(scope, data.domain, data.userId);
      if (!consent.ok) {
        return Err(consent.error);
      }
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        const row = await prisma.item.create({
          data: {
            domain: data.domain,
            userId: data.userId,
            plaidItemId: data.plaidItemId,
            plaidInstitutionId: data.plaidInstitutionId,
            institutionName: data.institutionName,
            encryptedAccessToken: asBuffer(data.encryptedAccessToken),
          },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async readEncryptedToken(scope, id): Promise<Result<EncryptedBlob, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        const row = await prisma.item.findFirst({
          where: whereForScope(scope, { id, removedAt: null }),
          select: { encryptedAccessToken: true },
        });
        if (row === null) {
          return Err({ kind: 'not_found' });
        }
        return Ok(asBytes(row.encryptedAccessToken) as EncryptedBlob);
      } catch (cause: unknown) {
        // We deliberately do NOT include the original exception message in
        // logs or errors here. The encryptedAccessToken column might appear
        // in stack traces from the SQLite driver; keeping the error
        // un-detailed prevents accidental token bytes leakage.
        return Err(mapPrismaError(cause));
      }
    },

    async rewriteEncryptedToken(scope, args): Promise<Result<void, RepoError>> {
      // Rewriting tokens is an admin-only operation (master rotation flow).
      if (scope.kind !== 'admin') {
        return Err({ kind: 'not_found' });
      }
      try {
        // Update gated by the same scope filter (admin → no extra filter).
        const r = await prisma.item.updateMany({
          where: whereForScope(scope, { id: args.id }),
          data: { encryptedAccessToken: asBuffer(args.newBlob) },
        });
        if (r.count === 0) {
          return Err({ kind: 'not_found' });
        }
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async updateSyncCursor(scope, args): Promise<Result<void, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        // For "success" we use updateMany with a flat data payload so the
        // scope filter is applied in one statement; for "error" / "pending"
        // we want to atomically increment consecutiveFailures, which Prisma
        // expresses via the singular `update`. To keep the scope guard, we
        // first verify the row is visible to the scope, then perform the
        // increment by primary key inside the same transaction.
        if (args.outcome === 'success') {
          const r = await prisma.item.updateMany({
            where: whereForScope(scope, { id: args.id }),
            data: {
              syncCursor: args.cursor,
              lastSyncAt: new Date(),
              lastSyncOutcome: 'success',
              consecutiveFailures: 0,
            },
          });
          if (r.count === 0) {
            return Err({ kind: 'not_found' });
          }
          return Ok(undefined);
        }

        const result = await prisma.$transaction(async (tx) => {
          const visible = await tx.item.findFirst({
            where: whereForScope(scope, { id: args.id }),
            select: { id: true },
          });
          if (visible === null) {
            return { found: false } as const;
          }
          await tx.item.update({
            where: { id: visible.id },
            data: {
              syncCursor: args.cursor,
              lastSyncAt: new Date(),
              lastSyncOutcome: args.outcome,
              consecutiveFailures: { increment: 1 },
            },
          });
          return { found: true } as const;
        });
        if (!result.found) {
          return Err({ kind: 'not_found' });
        }
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async softRemove(scope, args): Promise<Result<void, RepoError>> {
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        const r = await prisma.item.updateMany({
          where: whereForScope(scope, { id: args.id, removedAt: null }),
          data: { removedAt: new Date(), removedReason: args.reason },
        });
        if (r.count === 0) {
          return Err({ kind: 'not_found' });
        }
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}

/**
 * Verify the caller's scope is allowed to write a row with the given domain
 * and userId. Out-of-scope writes return `not_found` (NOT `unauthorized`)
 * to keep failure modes indistinguishable from "row simply doesn't exist".
 */
function consentToWrite(
  scope: RepoScope,
  rowDomain: Domain,
  rowUserId: UserId | null,
): Result<void, RepoError> {
  switch (scope.kind) {
    case 'admin':
      return Ok(undefined);
    case 'personal':
      if (rowDomain !== 'personal' || rowUserId !== scope.userId) {
        return Err({ kind: 'not_found' });
      }
      return Ok(undefined);
    case 'pcc':
      if (rowDomain !== 'pcc') {
        return Err({ kind: 'not_found' });
      }
      return Ok(undefined);
  }
}
