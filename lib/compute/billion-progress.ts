// Greylock — $1B progress (pure)
// =============================================================================
// AGENT-COMPUTE owns this file. No I/O.
//
// Goal is fixed at $1B = 1_000_000_000 dollars = 100_000_000_000 cents
// (= 1e11 cents). Progress is a Number in [0, 1]:
//   - Negative netWorth -> progress = 0
//   - netWorth >= goal  -> progress = 1
//   - Otherwise         -> netWorth / goal
//
// Precision note: at v0.1 net worth is far below 9.007e15 (the largest exact
// integer in IEEE-754 double), so naive `Number(netWorthCents) /
// Number(goalCents)` is fine. We still use a bigint-first computation to
// preserve four decimal places of precision in case future versions push
// past that bound — see ARCHITECTURE.md §6 fixture testing.
// =============================================================================

import type { BillionProgressResult, Cents } from '../types/domain.js';

const GOAL_CENTS = 100_000_000_000n; // $1,000,000,000.00 in cents

const PRECISION_FACTOR = 10_000n; // 4 decimal places of precision

export const billionProgress = (input: { readonly netWorthCents: Cents }): BillionProgressResult => {
  const nw = input.netWorthCents;

  let progress: number;
  if (nw <= 0n) {
    progress = 0;
  } else if (nw >= GOAL_CENTS) {
    progress = 1;
  } else {
    // bigint-first division preserves precision for very large nw values.
    // (nw * 10000n / goal) yields an integer in [0, 9999], divided by 10000
    // gives 4-decimal-place progress in [0, 0.9999].
    const scaled = (nw * PRECISION_FACTOR) / GOAL_CENTS;
    progress = Number(scaled) / Number(PRECISION_FACTOR);
  }

  return {
    netWorthCents: nw,
    goalCents: GOAL_CENTS as Cents,
    progress,
  };
};
