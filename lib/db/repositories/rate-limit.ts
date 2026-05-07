// Greylock — RateLimitRepository
// =============================================================================
// AGENT-DB. Implements `RateLimitRepository` from `lib/auth/rate-limit.ts`
// (the interface AGENT-AUTH owns). Fixed-window bucket with atomic
// upsert+increment inside a Prisma `$transaction` — guarantees two
// concurrent callers can't both observe "below cap" before either commits.
//
// Strategy:
//   1. Begin tx.
//   2. SELECT the row by bucketKey.
//   3. Run the pure `evaluateBucket()` from lib/auth/rate-limit.ts to decide.
//   4. UPSERT the row with the decided window/count.
//   5. Return outcome + the post-write row.
//
// SQLite's WAL + a write lock around the tx serializes concurrent writers
// against the same DB. This is sufficient for the localhost-only deployment.
// =============================================================================

import type { PrismaClient, RateLimitBucket as PrismaRateLimitBucket } from '@prisma/client';

import { Err, Ok } from '../../types/domain.js';
import type { Result } from '../../types/domain.js';

import {
  evaluateBucket,
  type RateLimitBucketRow,
  type RateLimitRepository,
} from '../../auth/rate-limit.js';

import { mapPrismaError } from './_shared.js';

function toRow(r: PrismaRateLimitBucket): RateLimitBucketRow {
  return {
    bucketKey: r.bucketKey,
    windowStart: r.windowStart,
    count: r.count,
  };
}

export interface CreateRateLimitRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createRateLimitRepository(
  input: CreateRateLimitRepositoryInput,
): RateLimitRepository {
  const { prisma } = input;

  return {
    async consumeOrTrip(
      args,
    ): Promise<
      Result<
        { readonly outcome: 'consumed' | 'tripped'; readonly row: RateLimitBucketRow },
        { kind: 'storage_failure' }
      >
    > {
      try {
        const result = await prisma.$transaction(async (tx) => {
          const existing = await tx.rateLimitBucket.findUnique({
            where: { bucketKey: args.bucketKey },
          });
          const decision = evaluateBucket({
            existing: existing === null ? null : toRow(existing),
            now: args.now,
            windowMinutes: args.windowMinutes,
            cap: args.cap,
          });

          // Always write through, so readers (and the next caller) see the
          // current windowStart even on "tripped".
          const upserted = await tx.rateLimitBucket.upsert({
            where: { bucketKey: args.bucketKey },
            create: {
              bucketKey: args.bucketKey,
              windowStart: decision.nextWindowStart,
              count: decision.nextCount,
            },
            update: {
              windowStart: decision.nextWindowStart,
              count: decision.nextCount,
            },
          });

          return { outcome: decision.outcome, row: toRow(upserted) };
        });
        return Ok(result);
      } catch (cause: unknown) {
        const mapped = mapPrismaError(cause);
        return Err(mapped.kind === 'storage_failure' ? mapped : { kind: 'storage_failure' });
      }
    },
  };
}
