// Greylock — month-net compute (pure)
// =============================================================================
// AGENT-COMPUTE owns this file. No I/O. `now` comes in as a parameter.
//
// Window: [now - 30 days, now). Inclusive at start, exclusive at end.
//
// Plaid sign convention (preserved by `lib/plaid/mappers.ts`):
//   +amountCents = OUTFLOW (money leaving the user)
//   -amountCents = INFLOW  (money arriving)
//
//   inflowCents  = Σ ( -amountCents ) over { tx | amountCents <  0n }
//                = Σ ( |amountCents| ) over inflows  (always >= 0)
//   outflowCents = Σ (  amountCents ) over { tx | amountCents >  0n }
//                                                   (always >= 0)
//   netCents     = inflowCents - outflowCents
//
// Pending transactions are skipped. Removed transactions (`removedAt !== null`)
// are skipped. Zero-amount transactions don't contribute to either bucket.
// =============================================================================

import type { Cents, MonthNetResult, Transaction } from '../types/domain.js';

const THIRTY_DAYS_MS = 30 * 24 * 60 * 60 * 1000;

export const monthNet = (input: {
  readonly transactions: ReadonlyArray<Transaction>;
  readonly now: Date;
}): MonthNetResult => {
  const nowMs = input.now.getTime();
  const startMs = nowMs - THIRTY_DAYS_MS;

  let inflow = 0n;
  let outflow = 0n;

  for (const tx of input.transactions) {
    if (tx.pending) {
      continue;
    }
    if (tx.removedAt !== null) {
      continue;
    }

    const txMs = tx.date.getTime();
    // [start, now): inclusive at start, exclusive at end.
    if (txMs < startMs) {
      continue;
    }
    if (txMs >= nowMs) {
      continue;
    }

    const amount = tx.amountCents;
    if (amount < 0n) {
      // Plaid sign: negative = inflow. Flip to positive magnitude.
      inflow += -amount;
    } else if (amount > 0n) {
      outflow += amount;
    }
    // amount === 0n: skip (no-op transaction).
  }

  const inflowCents = inflow as Cents;
  const outflowCents = outflow as Cents;
  const netCents = (inflow - outflow) as Cents;

  return {
    windowStart: new Date(startMs),
    windowEnd: new Date(nowMs),
    inflowCents,
    outflowCents,
    netCents,
  };
};
