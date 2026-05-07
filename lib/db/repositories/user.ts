// Greylock — UserRepository
// =============================================================================
// AGENT-DB. Concrete implementation of `UserRepository` from
// `lib/types/services.ts`. Backed by Prisma + SQLCipher.
//
// User rows do not carry a domain — they are identity rows. They are not
// scope-filtered. (PCC visibility applies to data rows like Item / Account /
// Transaction / Snapshot, not to who the users themselves are.)
// =============================================================================

import type { PrismaClient, User as PrismaUser } from '@prisma/client';

import { Err, Ok, UserId } from '../../types/domain.js';
import type { EncryptedBlob, RepoError, Result, Role, User } from '../../types/domain.js';
import type { UserRepository } from '../../types/services.js';

import { asBuffer, asBytes, mapPrismaError, tryDb } from './_shared.js';

function toDomain(row: PrismaUser): User {
  return {
    id: UserId(row.id),
    email: row.email,
    displayName: row.displayName,
    role: row.role as Role,
    userDekVersion: row.userDekVersion,
    createdAt: row.createdAt,
    updatedAt: row.updatedAt,
  };
}

export interface CreateUserRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createUserRepository(input: CreateUserRepositoryInput): UserRepository {
  const { prisma } = input;

  return {
    async findByEmail(email: string): Promise<Result<User | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.user.findUnique({
          where: { email: email.trim().toLowerCase() },
        });
        return row === null ? null : toDomain(row);
      });
    },

    async findById(id: UserId): Promise<Result<User | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.user.findUnique({ where: { id } });
        return row === null ? null : toDomain(row);
      });
    },

    async list(): Promise<Result<ReadonlyArray<User>, RepoError>> {
      return tryDb(async () => {
        const rows = await prisma.user.findMany({ orderBy: { createdAt: 'asc' } });
        return rows.map(toDomain);
      });
    },

    async create(args: {
      readonly email: string;
      readonly displayName: string;
      readonly role: Role;
    }): Promise<Result<User, RepoError>> {
      try {
        const row = await prisma.user.create({
          data: {
            email: args.email.trim().toLowerCase(),
            displayName: args.displayName,
            role: args.role,
          },
        });
        return Ok(toDomain(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },

    async setWrappedUserDek(args: {
      readonly userId: UserId;
      readonly version: number;
      readonly wrapped: EncryptedBlob;
    }): Promise<Result<void, RepoError>> {
      try {
        await prisma.user.update({
          where: { id: args.userId },
          data: {
            wrappedUserDek: asBuffer(args.wrapped),
            userDekVersion: args.version,
          },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}

// -----------------------------------------------------------------------------
// Helpers exported for callers that need wrappedUserDek / kekSalt off-band.
// AGENT-AUTH reads these to derive per-user KEK at session establishment.
// They are NOT part of the canonical UserRepository contract, but live here
// so all `User` reads still go through `lib/db/`.
// -----------------------------------------------------------------------------

export interface UserAuthMaterial {
  readonly userId: UserId;
  readonly userDekVersion: number;
  /** Wrapped DEK bytes; null until the user has completed enrollment. */
  readonly wrappedUserDek: EncryptedBlob | null;
}

export async function readUserAuthMaterial(args: {
  readonly prisma: PrismaClient;
  readonly userId: UserId;
}): Promise<Result<UserAuthMaterial | null, RepoError>> {
  return tryDb(async () => {
    const row = await args.prisma.user.findUnique({
      where: { id: args.userId },
      select: { id: true, userDekVersion: true, wrappedUserDek: true },
    });
    if (row === null) {
      return null;
    }
    return {
      userId: UserId(row.id),
      userDekVersion: row.userDekVersion,
      wrappedUserDek:
        row.wrappedUserDek === null ? null : (asBytes(row.wrappedUserDek) as EncryptedBlob),
    };
  });
}
