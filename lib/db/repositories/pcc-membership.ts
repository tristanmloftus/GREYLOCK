// Greylock — PccMembershipRepository
// =============================================================================
// AGENT-DB. Concrete implementation of `PccMembershipRepository`. Used by:
//   - admin CLI to add/revoke members,
//   - other repositories indirectly (PCC-scope reads first call
//     `requirePccMembershipOrNotFound` which queries this table directly via
//     the shared TxClient — kept inline rather than calling this repo, to
//     avoid an extra Promise/Result hop in the hot path).
// =============================================================================

import type { PrismaClient, PccMembership as PrismaPccMembership } from '@prisma/client';

import { Err, Ok, UserId } from '../../types/domain.js';
import type { PccMembership, RepoError, Result } from '../../types/domain.js';
import type { PccMembershipRepository } from '../../types/services.js';

import { mapPrismaError, tryDb } from './_shared.js';

function toDomain(row: PrismaPccMembership): PccMembership {
  return {
    userId: UserId(row.userId),
    joinedAt: row.joinedAt,
    revokedAt: row.revokedAt,
  };
}

export interface CreatePccMembershipRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createPccMembershipRepository(
  input: CreatePccMembershipRepositoryInput,
): PccMembershipRepository {
  const { prisma } = input;

  return {
    async isActiveMember(userId: UserId): Promise<Result<boolean, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.pccMembership.findFirst({
          where: { userId, revokedAt: null },
          select: { id: true },
        });
        return row !== null;
      });
    },

    async list(): Promise<Result<ReadonlyArray<PccMembership>, RepoError>> {
      return tryDb(async () => {
        const rows = await prisma.pccMembership.findMany({
          orderBy: { joinedAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },

    async add(args): Promise<Result<PccMembership, RepoError>> {
      try {
        const row = await prisma.pccMembership.upsert({
          where: { userId: args.userId },
          // If the row already exists (possibly revoked), un-revoke it.
          update: { revokedAt: null },
          create: { userId: args.userId },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async revoke(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.pccMembership.update({
          where: { userId: args.userId },
          data: { revokedAt: new Date() },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}
