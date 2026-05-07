// Greylock — SnapshotRepository
// =============================================================================
// AGENT-DB. Snapshots carry their own (domain, userId) tuple. Visibility:
//   - personal scope: WHERE domain='personal' AND userId = scope.userId
//   - pcc scope:      WHERE domain='pcc' AND scope's user is active member
//   - admin scope:    no extra filter
// =============================================================================

import type {
  PrismaClient,
  NetWorthSnapshot as PrismaNetWorthSnapshot,
  Prisma,
} from '@prisma/client';

import { Err, Ok, SnapshotId, UserId } from '../../types/domain.js';
import type { Domain, NetWorthSnapshot, RepoError, Result } from '../../types/domain.js';
import type { RepoScope, SnapshotRepository } from '../../types/services.js';

import { mapPrismaError, requirePccMembershipOrNotFound, tryDb } from './_shared.js';

function toDomain(row: PrismaNetWorthSnapshot): NetWorthSnapshot {
  return {
    id: SnapshotId(row.id),
    domain: row.domain as Domain,
    userId: row.userId === null ? null : UserId(row.userId),
    takenAt: row.takenAt,
    assetsCents: row.assetsCents,
    liabilitiesCents: row.liabilitiesCents,
    netWorthCents: row.netWorthCents,
    cashCents: row.cashCents,
    monthNetCents: row.monthNetCents,
    computeVersion: row.computeVersion,
    breakdownJson: row.breakdownJson,
  };
}

export interface CreateSnapshotRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createSnapshotRepository(input: CreateSnapshotRepositoryInput): SnapshotRepository {
  const { prisma } = input;

  /** Build the visibility WHERE clause and merge optional extras. */
  function whereForScope(
    scope: RepoScope,
    extra: Prisma.NetWorthSnapshotWhereInput = {},
  ): Prisma.NetWorthSnapshotWhereInput {
    switch (scope.kind) {
      case 'admin':
        return { ...extra };
      case 'personal':
        return { domain: 'personal', userId: scope.userId, ...extra };
      case 'pcc':
        return { domain: 'pcc', ...extra };
    }
  }

  /** Caller must have consent to write a row of this (domain, userId). */
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

  return {
    async insert(scope, input2): Promise<Result<NetWorthSnapshot, RepoError>> {
      const consent = consentToWrite(scope, input2.domain, input2.userId);
      if (!consent.ok) {
        return Err(consent.error);
      }
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      try {
        const row = await prisma.netWorthSnapshot.create({
          data: {
            domain: input2.domain,
            userId: input2.userId,
            takenAt: input2.takenAt,
            assetsCents: input2.assetsCents,
            liabilitiesCents: input2.liabilitiesCents,
            netWorthCents: input2.netWorthCents,
            cashCents: input2.cashCents,
            monthNetCents: input2.monthNetCents,
            computeVersion: input2.computeVersion,
            breakdownJson: input2.breakdownJson,
          },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async latest(scope, args): Promise<Result<NetWorthSnapshot | null, RepoError>> {
      const consent = consentToWrite(scope, args.domain, args.userId);
      if (!consent.ok) {
        return Err(consent.error);
      }
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const where = whereForScope(scope, {
          domain: args.domain,
          userId: args.userId,
        });
        const row = await prisma.netWorthSnapshot.findFirst({
          where,
          orderBy: { takenAt: 'desc' },
        });
        return row === null ? null : toDomain(row);
      });
    },

    async series(scope, args): Promise<Result<ReadonlyArray<NetWorthSnapshot>, RepoError>> {
      const consent = consentToWrite(scope, args.domain, args.userId);
      if (!consent.ok) {
        return Err(consent.error);
      }
      const gate = await requirePccMembershipOrNotFound({ scope, prisma });
      if (!gate.ok) {
        return Err(gate.error);
      }
      return tryDb(async () => {
        const where = whereForScope(scope, {
          domain: args.domain,
          userId: args.userId,
          takenAt: { gte: args.fromTs, lt: args.toTs },
        });
        const rows = await prisma.netWorthSnapshot.findMany({
          where,
          orderBy: { takenAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },
  };
}
