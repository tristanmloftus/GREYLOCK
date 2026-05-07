// Greylock — rate-limit pure logic + bucket-key tests
// =============================================================================
// AGENT-AUTH (Phase 2). Pure unit tests of `evaluateBucket` (the body of the
// `consumeOrTrip` Prisma transaction AGENT-DB will implement) and the
// bucket-key helper. No DB needed — `consume()` is exercised against an
// in-memory mock repo.
// =============================================================================

import { describe, expect, it } from 'vitest';

import { Ok } from '../../../lib/types/domain.js';
import {
  consume,
  evaluateBucket,
  rateLimitBucketKey,
  type RateLimitBucketRow,
  type RateLimitRepository,
} from '../../../lib/auth/rate-limit.js';

const config = { cap: 5, windowMinutes: 15 } as const;

describe('rate-limit / bucket key', () => {
  it('lowercases email and falls back to "unknown" IP', () => {
    expect(
      rateLimitBucketKey({ flow: 'auth:assert', remoteAddr: null, email: ' Rory@X.com ' }),
    ).toBe('auth:assert:unknown:rory@x.com');
  });

  it('separates buckets per (IP, email)', () => {
    const a = rateLimitBucketKey({ flow: 'auth:assert', remoteAddr: '1.1.1.1', email: 'a@b.com' });
    const b = rateLimitBucketKey({ flow: 'auth:assert', remoteAddr: '1.1.1.1', email: 'c@b.com' });
    const c = rateLimitBucketKey({ flow: 'auth:assert', remoteAddr: '2.2.2.2', email: 'a@b.com' });
    expect(new Set([a, b, c]).size).toBe(3);
  });
});

describe('rate-limit / evaluateBucket pure logic', () => {
  it('opens a fresh window when no row exists', () => {
    const now = new Date('2026-01-01T00:00:00Z');
    const out = evaluateBucket({ existing: null, now, ...config });
    expect(out.outcome).toBe('consumed');
    expect(out.nextCount).toBe(1);
    expect(out.nextWindowStart).toEqual(now);
  });

  it('opens a fresh window when the existing row is past its window', () => {
    const now = new Date('2026-01-01T00:30:00Z');
    const stale: RateLimitBucketRow = {
      bucketKey: 'k',
      windowStart: new Date('2026-01-01T00:00:00Z'),
      count: 5,
    };
    const out = evaluateBucket({ existing: stale, now, ...config });
    expect(out.outcome).toBe('consumed');
    expect(out.nextCount).toBe(1);
    expect(out.nextWindowStart).toEqual(now);
  });

  it('increments within the window', () => {
    const now = new Date('2026-01-01T00:05:00Z');
    const row: RateLimitBucketRow = {
      bucketKey: 'k',
      windowStart: new Date('2026-01-01T00:00:00Z'),
      count: 2,
    };
    const out = evaluateBucket({ existing: row, now, ...config });
    expect(out.outcome).toBe('consumed');
    expect(out.nextCount).toBe(3);
    expect(out.nextWindowStart).toEqual(row.windowStart);
  });

  it('trips when next > cap', () => {
    const now = new Date('2026-01-01T00:05:00Z');
    const row: RateLimitBucketRow = {
      bucketKey: 'k',
      windowStart: new Date('2026-01-01T00:00:00Z'),
      count: 5,
    };
    const out = evaluateBucket({ existing: row, now, ...config });
    expect(out.outcome).toBe('tripped');
    expect(out.nextCount).toBe(5); // unchanged on trip
  });
});

describe('rate-limit / consume() integration with a mock repo', () => {
  function mockRepo(rows: Map<string, RateLimitBucketRow> = new Map()): RateLimitRepository {
    return {
      consumeOrTrip: async (input) => {
        const existing = rows.get(input.bucketKey) ?? null;
        const evalOut = evaluateBucket({
          existing,
          now: input.now,
          windowMinutes: input.windowMinutes,
          cap: input.cap,
        });
        const next: RateLimitBucketRow = {
          bucketKey: input.bucketKey,
          windowStart: evalOut.nextWindowStart,
          count: evalOut.nextCount,
        };
        rows.set(input.bucketKey, next);
        return Ok({ outcome: evalOut.outcome, row: next });
      },
    };
  }

  it('allows 5 consumes then trips on the 6th', async () => {
    const repo = mockRepo();
    const now = new Date('2026-01-01T00:00:00Z');
    for (let i = 0; i < 5; i += 1) {
      const r = await consume({ repo, bucketKey: 'k', now, config });
      expect(r.ok).toBe(true);
      if (r.ok) {
        expect(r.value.kind).toBe('allowed');
      }
    }
    const tripped = await consume({ repo, bucketKey: 'k', now, config });
    expect(tripped.ok).toBe(true);
    if (tripped.ok) {
      expect(tripped.value.kind).toBe('tripped');
      if (tripped.value.kind === 'tripped') {
        expect(tripped.value.retryAfterSeconds).toBeGreaterThan(0);
        expect(tripped.value.retryAfterSeconds).toBeLessThanOrEqual(15 * 60);
      }
    }
  });

  it('rolls over after the window expires', async () => {
    const repo = mockRepo();
    const t0 = new Date('2026-01-01T00:00:00Z');
    for (let i = 0; i < 5; i += 1) {
      await consume({ repo, bucketKey: 'k', now: t0, config });
    }
    const t1 = new Date('2026-01-01T00:15:01Z'); // just past window
    const r = await consume({ repo, bucketKey: 'k', now: t1, config });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.value.kind).toBe('allowed');
    }
  });

  it('uses separate buckets per (IP, email)', async () => {
    const repo = mockRepo();
    const now = new Date('2026-01-01T00:00:00Z');
    for (let i = 0; i < 5; i += 1) {
      await consume({ repo, bucketKey: 'auth:assert:1.1.1.1:a@b.com', now, config });
    }
    const otherBucket = await consume({
      repo,
      bucketKey: 'auth:assert:1.1.1.1:c@b.com',
      now,
      config,
    });
    expect(otherBucket.ok).toBe(true);
    if (otherBucket.ok) {
      expect(otherBucket.value.kind).toBe('allowed');
    }
  });
});
