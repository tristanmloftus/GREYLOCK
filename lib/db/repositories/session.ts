// Greylock — SessionRepository
// =============================================================================
// AGENT-DB. Concrete implementation of `SessionRepository`.
//
// Session rows belong to one user and are queried by id or by userId. They
// are not scope-filtered (the caller — AuthService — is the only one that
// reads sessions). Reads explicitly filter `status = active` AND non-expired
// timestamps so a "find" on a revoked / expired row returns null.
// =============================================================================

import type { PrismaClient, Session as PrismaSession } from '@prisma/client';

import { Err, Ok, SessionId, UserId } from '../../types/domain.js';
import type { RepoError, Result, Session, SessionStatus } from '../../types/domain.js';
import type { SessionRepository } from '../../types/services.js';

import { mapPrismaError, tryDb } from './_shared.js';

function toDomain(row: PrismaSession): Session {
  return {
    id: SessionId(row.id),
    userId: UserId(row.userId),
    status: row.status as SessionStatus,
    createdAt: row.createdAt,
    lastActivityAt: row.lastActivityAt,
    expiresAt: row.expiresAt,
    idleTimeoutAt: row.idleTimeoutAt,
    revokedAt: row.revokedAt,
    revokedReason: row.revokedReason,
    userAgent: row.userAgent,
    remoteAddr: row.remoteAddr,
  };
}

export interface CreateSessionRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createSessionRepository(input: CreateSessionRepositoryInput): SessionRepository {
  const { prisma } = input;

  return {
    async create(args): Promise<Result<Session, RepoError>> {
      try {
        const row = await prisma.session.create({
          data: {
            userId: args.userId,
            expiresAt: args.expiresAt,
            idleTimeoutAt: args.idleTimeoutAt,
            userAgent: args.userAgent,
            remoteAddr: args.remoteAddr,
          },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async findActiveById(id: SessionId): Promise<Result<Session | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.session.findFirst({
          where: { id, status: 'active' },
        });
        return row === null ? null : toDomain(row);
      });
    },

    async findActiveByUser(userId: UserId): Promise<Result<Session | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.session.findFirst({
          where: { userId, status: 'active' },
          orderBy: { createdAt: 'desc' },
        });
        return row === null ? null : toDomain(row);
      });
    },

    async touch(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.session.update({
          where: { id: args.id },
          data: { lastActivityAt: new Date(), idleTimeoutAt: args.newIdleTimeoutAt },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async revoke(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.session.update({
          where: { id: args.id },
          data: { status: 'revoked', revokedAt: new Date(), revokedReason: args.reason },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async revokeAllActive(args): Promise<Result<{ readonly count: number }, RepoError>> {
      return tryDb(async () => {
        const r = await prisma.session.updateMany({
          where: { status: 'active' },
          data: { status: 'revoked', revokedAt: new Date(), revokedReason: args.reason },
        });
        return { count: r.count };
      });
    },

    async expireOverdue(now: Date): Promise<Result<{ readonly count: number }, RepoError>> {
      return tryDb(async () => {
        const r = await prisma.session.updateMany({
          where: {
            status: 'active',
            OR: [{ expiresAt: { lte: now } }, { idleTimeoutAt: { lte: now } }],
          },
          data: { status: 'expired' },
        });
        return { count: r.count };
      });
    },
  };
}
