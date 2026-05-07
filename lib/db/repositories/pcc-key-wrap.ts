// Greylock — PccKeyWrapRepository
// =============================================================================
// AGENT-DB. Storage for the wrapped PCC DEK and its KDF parameters. There is
// always exactly one *active* wrap (the row with the highest `version` and
// `retiredAt IS NULL`). Master rotation inserts a new wrap and retires the
// old in one transaction.
//
// Used by AGENT-CRYPTO via callback during boot (`unwrapPccDek`) and during
// `rotateMaster()`. AGENT-CRYPTO never imports Prisma.
// =============================================================================

import type { PrismaClient, PccKeyWrap as PrismaPccKeyWrap } from '@prisma/client';

import { Err, Ok } from '../../types/domain.js';
import type { EncryptedBlob, RepoError, Result } from '../../types/domain.js';

import { asBuffer, asBytes, mapPrismaError, tryDb } from './_shared.js';

export interface PccKeyWrapRecord {
  readonly id: string;
  readonly version: number;
  readonly wrappedDek: EncryptedBlob;
  readonly kdfAlgorithm: string;
  readonly kdfN: number;
  readonly kdfR: number;
  readonly kdfP: number;
  readonly kdfSalt: Uint8Array;
  readonly createdAt: Date;
  readonly retiredAt: Date | null;
}

function toRecord(row: PrismaPccKeyWrap): PccKeyWrapRecord {
  return {
    id: row.id,
    version: row.version,
    wrappedDek: asBytes(row.wrappedDek) as EncryptedBlob,
    kdfAlgorithm: row.kdfAlgorithm,
    kdfN: row.kdfN,
    kdfR: row.kdfR,
    kdfP: row.kdfP,
    kdfSalt: asBytes(row.kdfSalt),
    createdAt: row.createdAt,
    retiredAt: row.retiredAt,
  };
}

export interface PccKeyWrapRepository {
  /** Latest active (un-retired) wrap. Null if no wrap exists yet. */
  findActive(): Promise<Result<PccKeyWrapRecord | null, RepoError>>;
  /** Lookup a specific version (used during rotation to load the previous
   *  wrap before retiring it). */
  findByVersion(version: number): Promise<Result<PccKeyWrapRecord | null, RepoError>>;
  /** Insert a new wrap row. Caller is responsible for monotonic version. */
  insert(input: {
    readonly version: number;
    readonly wrappedDek: EncryptedBlob;
    readonly kdfAlgorithm: 'scrypt';
    readonly kdfN: number;
    readonly kdfR: number;
    readonly kdfP: number;
    readonly kdfSalt: Uint8Array;
  }): Promise<Result<PccKeyWrapRecord, RepoError>>;
  /** Retire a previously-active wrap. */
  retire(input: { readonly version: number }): Promise<Result<void, RepoError>>;
}

export interface CreatePccKeyWrapRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createPccKeyWrapRepository(
  input: CreatePccKeyWrapRepositoryInput,
): PccKeyWrapRepository {
  const { prisma } = input;
  return {
    async findActive(): Promise<Result<PccKeyWrapRecord | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.pccKeyWrap.findFirst({
          where: { retiredAt: null },
          orderBy: { version: 'desc' },
        });
        return row === null ? null : toRecord(row);
      });
    },
    async findByVersion(version): Promise<Result<PccKeyWrapRecord | null, RepoError>> {
      return tryDb(async () => {
        const row = await prisma.pccKeyWrap.findUnique({ where: { version } });
        return row === null ? null : toRecord(row);
      });
    },
    async insert(args): Promise<Result<PccKeyWrapRecord, RepoError>> {
      try {
        const row = await prisma.pccKeyWrap.create({
          data: {
            version: args.version,
            wrappedDek: asBuffer(args.wrappedDek),
            kdfAlgorithm: args.kdfAlgorithm,
            kdfN: args.kdfN,
            kdfR: args.kdfR,
            kdfP: args.kdfP,
            kdfSalt: asBuffer(args.kdfSalt),
          },
        });
        return Ok(toRecord(row));
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
    async retire(args): Promise<Result<void, RepoError>> {
      try {
        await prisma.pccKeyWrap.update({
          where: { version: args.version },
          data: { retiredAt: new Date() },
        });
        return Ok(undefined);
      } catch (cause: unknown) {
        return Err(mapPrismaError(cause));
      }
    },
  };
}
