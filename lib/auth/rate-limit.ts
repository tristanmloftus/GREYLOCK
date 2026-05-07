// Greylock — fixed-window auth rate limiter
// =============================================================================
// AGENT-AUTH (Phase 2). Bucket key shape:
//   auth:assert:<ip>:<email-lower>     — authentication begin/complete
//   auth:enroll:<ip>:<email-lower>     — registration begin/complete
//
// Strategy: fixed window of `AUTH_RATE_LIMIT_WINDOW_MINUTES` (default 15) and
// cap of `AUTH_RATE_LIMIT_ATTEMPTS` (default 5). On each `consume()` call:
//   1. Read the bucket. If absent OR `windowStart + window <= now` → reset to
//      windowStart=now, count=1.
//   2. Else if `count + 1 > cap` → trip (return `tripped`).
//   3. Else increment.
//
// Storage is delegated to a small repository interface below — AGENT-DB will
// implement it against the `RateLimitBucket` Prisma model. We define the
// interface here (not in `lib/types/services.ts`) so AGENT-DB can ship its
// other repositories independently and the orchestrator can rationalize the
// contract at Phase 3.
// =============================================================================

import { Err, Ok } from '../types/domain.js';
import type { Result } from '../types/domain.js';

// -----------------------------------------------------------------------------
// Storage interface (AGENT-DB will implement)
// -----------------------------------------------------------------------------

export interface RateLimitBucketRow {
  readonly bucketKey: string;
  readonly windowStart: Date;
  readonly count: number;
}

export interface RateLimitRepository {
  /** Atomic upsert + count increment. Implementation MUST be transactional so
   *  two concurrent callers cannot both see "count below cap" before either
   *  commits. Returns the post-write row. */
  consumeOrTrip(input: {
    readonly bucketKey: string;
    readonly now: Date;
    readonly windowMinutes: number;
    readonly cap: number;
  }): Promise<Result<{ readonly outcome: 'consumed' | 'tripped'; readonly row: RateLimitBucketRow }, { kind: 'storage_failure' }>>;
}

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

export interface RateLimitConfig {
  readonly cap: number;
  readonly windowMinutes: number;
}

export function readRateLimitConfig(): RateLimitConfig {
  const capStr = process.env['AUTH_RATE_LIMIT_ATTEMPTS'] ?? '5';
  const winStr = process.env['AUTH_RATE_LIMIT_WINDOW_MINUTES'] ?? '15';
  const cap = Number.parseInt(capStr, 10);
  const windowMinutes = Number.parseInt(winStr, 10);
  if (!Number.isFinite(cap) || cap <= 0) {
    throw new Error('AUTH_RATE_LIMIT_ATTEMPTS must be a positive integer');
  }
  if (!Number.isFinite(windowMinutes) || windowMinutes <= 0) {
    throw new Error('AUTH_RATE_LIMIT_WINDOW_MINUTES must be a positive integer');
  }
  return { cap, windowMinutes };
}

// -----------------------------------------------------------------------------
// Bucket key helpers
// -----------------------------------------------------------------------------

export type RateLimitFlow = 'auth:assert' | 'auth:enroll';

export function rateLimitBucketKey(args: {
  readonly flow: RateLimitFlow;
  readonly remoteAddr: string | null;
  readonly email: string;
}): string {
  // Normalize: lowercase email; treat null IP as the literal 'unknown' token
  // so a missing remoteAddr still gets rate-limited (fail-secure).
  const ip = args.remoteAddr ?? 'unknown';
  const email = args.email.trim().toLowerCase();
  return `${args.flow}:${ip}:${email}`;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

export type RateLimitOutcome =
  | { readonly kind: 'allowed'; readonly remaining: number }
  | { readonly kind: 'tripped'; readonly retryAfterSeconds: number };

export async function consume(args: {
  readonly repo: RateLimitRepository;
  readonly bucketKey: string;
  readonly now: Date;
  readonly config: RateLimitConfig;
}): Promise<Result<RateLimitOutcome, { kind: 'storage_failure' }>> {
  const result = await args.repo.consumeOrTrip({
    bucketKey: args.bucketKey,
    now: args.now,
    windowMinutes: args.config.windowMinutes,
    cap: args.config.cap,
  });
  if (!result.ok) {
    return Err(result.error);
  }
  if (result.value.outcome === 'tripped') {
    const windowEndMs =
      result.value.row.windowStart.getTime() + args.config.windowMinutes * 60 * 1000;
    const retryAfterSeconds = Math.max(1, Math.ceil((windowEndMs - args.now.getTime()) / 1000));
    return Ok({ kind: 'tripped', retryAfterSeconds });
  }
  return Ok({ kind: 'allowed', remaining: Math.max(0, args.config.cap - result.value.row.count) });
}

// -----------------------------------------------------------------------------
// Pure logic (extracted for testability — AGENT-DB will use this as the body
// of its `consumeOrTrip` Prisma transaction).
// -----------------------------------------------------------------------------

export interface PureBucketEvaluation {
  readonly outcome: 'consumed' | 'tripped';
  readonly nextWindowStart: Date;
  readonly nextCount: number;
}

/** Pure evaluation of a single fixed-window decision. Inputs:
 *   - `existing`: current row, or null if missing.
 *   - `now`: clock.
 *   - `windowMinutes` / `cap`: config.
 *  Outputs the new window-start and count plus the outcome. Caller writes. */
export function evaluateBucket(args: {
  readonly existing: RateLimitBucketRow | null;
  readonly now: Date;
  readonly windowMinutes: number;
  readonly cap: number;
}): PureBucketEvaluation {
  const winMs = args.windowMinutes * 60 * 1000;
  const inWindow =
    args.existing !== null && args.existing.windowStart.getTime() + winMs > args.now.getTime();

  if (!inWindow) {
    return { outcome: 'consumed', nextWindowStart: args.now, nextCount: 1 };
  }

  const next = (args.existing?.count ?? 0) + 1;
  if (next > args.cap) {
    return {
      outcome: 'tripped',
      nextWindowStart: args.existing?.windowStart ?? args.now,
      nextCount: args.existing?.count ?? 0,
    };
  }
  return {
    outcome: 'consumed',
    nextWindowStart: args.existing?.windowStart ?? args.now,
    nextCount: next,
  };
}
