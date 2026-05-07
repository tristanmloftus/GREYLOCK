// Greylock — PasskeyRepository
// =============================================================================
// AGENT-DB. Concrete implementation of `PasskeyRepository`. WebAuthn passkeys
// are user-identity material — like User rows, they are not scope-filtered.
// AGENT-AUTH callers always look them up by either credentialId (during
// authentication) or userId (during admin operations).
// =============================================================================

import type { PrismaClient, Passkey as PrismaPasskey } from '@prisma/client';

import { Err, Ok, PasskeyId, UserId } from '../../types/domain.js';
import type { Passkey, RepoError, Result } from '../../types/domain.js';
import type { PasskeyRepository } from '../../types/services.js';

import { asBuffer, asBytes, mapPrismaError, tryDb } from './_shared.js';

function toDomain(row: PrismaPasskey): Passkey {
  return {
    id: PasskeyId(row.id),
    userId: UserId(row.userId),
    credentialId: asBytes(row.credentialId),
    credentialPublicKey: asBytes(row.credentialPublicKey),
    counter: row.counter,
    transports:
      row.transports === null ? [] : row.transports.split(',').filter((s) => s.length > 0),
    aaguid: row.aaguid === null ? null : asBytes(row.aaguid),
    deviceLabel: row.deviceLabel,
    createdAt: row.createdAt,
    lastUsedAt: row.lastUsedAt,
    revokedAt: row.revokedAt,
  };
}

export interface CreatePasskeyRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createPasskeyRepository(input: CreatePasskeyRepositoryInput): PasskeyRepository {
  const { prisma } = input;

  return {
    async findByCredentialId(credentialId: Uint8Array): Promise<Result<Passkey | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.passkey.findUnique({
          where: { credentialId: asBuffer(credentialId) },
        });
        return row === null ? null : toDomain(row);
      });
    },

    async listByUser(userId: UserId): Promise<Result<ReadonlyArray<Passkey>, RepoError>> {
      return tryDb(async () => {
        const rows = await prisma.passkey.findMany({
          where: { userId },
          orderBy: { createdAt: 'asc' },
        });
        return rows.map(toDomain);
      });
    },

    async create(args): Promise<Result<Passkey, RepoError>> {
      try {
        const row = await prisma.passkey.create({
          data: {
            userId: args.userId,
            credentialId: asBuffer(args.credentialId),
            credentialPublicKey: asBuffer(args.credentialPublicKey),
            counter: args.counter,
            transports: args.transports.length === 0 ? null : args.transports.join(','),
            aaguid: args.aaguid === null ? null : asBuffer(args.aaguid),
            deviceLabel: args.deviceLabel,
            kekSalt: asBuffer(args.kekSalt),
          },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async bumpCounter(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.passkey.update({
          where: { id: args.id },
          data: { counter: args.newCounter, lastUsedAt: new Date() },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async revoke(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.passkey.update({
          where: { id: args.id },
          data: { revokedAt: new Date() },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}

// -----------------------------------------------------------------------------
// Off-band helper for AGENT-AUTH: read kekSalt for KEK derivation. NOT part
// of the canonical Repository contract because the salt is auth-internal —
// only `lib/auth/` should call this.
// -----------------------------------------------------------------------------

export async function readPasskeyKekSalt(args: {
  readonly prisma: PrismaClient;
  readonly passkeyId: PasskeyId;
}): Promise<Result<Uint8Array | null, RepoError>> {
  return tryDb(async () => {
    const row = await args.prisma.passkey.findUnique({
      where: { id: args.passkeyId },
      select: { kekSalt: true },
    });
    return row === null ? null : asBytes(row.kekSalt);
  });
}
