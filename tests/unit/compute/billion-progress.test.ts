// Tests for `lib/compute/billion-progress.ts`.

import { describe, it, expect } from 'vitest';

import { billionProgress } from '../../../lib/compute/billion-progress.js';
import type { Cents } from '../../../lib/types/domain.js';

const ONE_BILLION_CENTS = 100_000_000_000n; // $1B in cents

describe('billionProgress — basic targets', () => {
  it('returns 0 for $0 net worth', () => {
    const r = billionProgress({ netWorthCents: 0n as Cents });
    expect(r.progress).toBe(0);
    expect(r.netWorthCents).toBe(0n);
    expect(r.goalCents).toBe(ONE_BILLION_CENTS);
  });

  it('returns 0.5 for $500M net worth', () => {
    const half = ONE_BILLION_CENTS / 2n;
    const r = billionProgress({ netWorthCents: half as Cents });
    expect(r.progress).toBeCloseTo(0.5, 4);
  });

  it('returns 1 at exactly $1B', () => {
    const r = billionProgress({ netWorthCents: ONE_BILLION_CENTS as Cents });
    expect(r.progress).toBe(1);
  });

  it('clamps to 1 for net worth exceeding $1B', () => {
    const tenB = ONE_BILLION_CENTS * 10n;
    const r = billionProgress({ netWorthCents: tenB as Cents });
    expect(r.progress).toBe(1);
    // Underlying value is preserved, only progress is clamped.
    expect(r.netWorthCents).toBe(tenB);
  });

  it('returns 0 for negative net worth', () => {
    const r = billionProgress({ netWorthCents: -100_000_000n as Cents });
    expect(r.progress).toBe(0);
  });
});

describe('billionProgress — small/edge values', () => {
  it('returns ~0 (not negative) for a tiny positive nw', () => {
    const r = billionProgress({ netWorthCents: 100n as Cents });
    expect(r.progress).toBeGreaterThanOrEqual(0);
    // 100 / 1e11 = 1e-9 -> with 4 decimal precision this rounds to 0.0000
    expect(r.progress).toBe(0);
  });

  it('represents 1% net worth as ~0.01', () => {
    const onePercent = ONE_BILLION_CENTS / 100n;
    const r = billionProgress({ netWorthCents: onePercent as Cents });
    expect(r.progress).toBeCloseTo(0.01, 4);
  });

  it('represents 99.99% net worth correctly', () => {
    const target = (ONE_BILLION_CENTS * 9999n) / 10000n;
    const r = billionProgress({ netWorthCents: target as Cents });
    expect(r.progress).toBeCloseTo(0.9999, 4);
  });
});

describe('billionProgress — large-value precision', () => {
  it('preserves precision for nw > 1e15 cents', () => {
    // 5e15 cents = $50T (well past JS-safe-int territory if we used Number).
    // Goal = 1e11. Ratio is 5e4, so it must clamp to 1.
    const huge = 5_000_000_000_000_000n;
    const r = billionProgress({ netWorthCents: huge as Cents });
    expect(r.progress).toBe(1);
    expect(r.netWorthCents).toBe(huge);
  });

  it('uses bigint-first division so progress is stable for awkward fractions', () => {
    // 1/3 of a billion. Result should be ~0.3333, not NaN, not Infinity.
    const oneThird = ONE_BILLION_CENTS / 3n;
    const r = billionProgress({ netWorthCents: oneThird as Cents });
    expect(r.progress).toBeGreaterThan(0.33);
    expect(r.progress).toBeLessThan(0.34);
  });
});
