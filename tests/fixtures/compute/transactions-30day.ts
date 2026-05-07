// Fixture: transactions spanning the 30-day window plus older.
//
// `now` is fixed at 2026-05-06T12:00:00Z (Phase 3 wall-clock at time of
// writing). The window is [now - 30d, now). Each transaction is keyed to a
// position relative to that window so test cases can assert boundary behavior.

import type { Transaction } from '../../../lib/types/domain.js';

import { buildTransaction } from './builders.js';

export const FIXTURE_NOW = new Date('2026-05-06T12:00:00.000Z');

const THIRTY_DAYS_MS = 30 * 24 * 60 * 60 * 1000;
export const FIXTURE_WINDOW_START = new Date(FIXTURE_NOW.getTime() - THIRTY_DAYS_MS);

// EXACTLY the inclusive start boundary -> included.
export const txAtStartBoundary: Transaction = buildTransaction({
  id: 'tx_start',
  amountCents: 1_000n, // $10 outflow
  date: new Date(FIXTURE_WINDOW_START.getTime()),
});

// One ms before the start -> excluded.
export const txOneMsBeforeStart: Transaction = buildTransaction({
  id: 'tx_before',
  amountCents: 9_999_999n, // huge outflow that should NOT count
  date: new Date(FIXTURE_WINDOW_START.getTime() - 1),
});

// Comfortably inside: 15 days before now, $200 outflow.
export const txOutflow200: Transaction = buildTransaction({
  id: 'tx_out200',
  amountCents: 20_000n,
  date: new Date(FIXTURE_NOW.getTime() - 15 * 24 * 60 * 60 * 1000),
});

// Inflow: 7 days before now, $5,000 paycheck (negative -> inflow).
export const txInflow5000: Transaction = buildTransaction({
  id: 'tx_in5000',
  amountCents: -500_000n,
  date: new Date(FIXTURE_NOW.getTime() - 7 * 24 * 60 * 60 * 1000),
});

// Pending — must be skipped even if inside window.
export const txPending: Transaction = buildTransaction({
  id: 'tx_pending',
  amountCents: 80_000n,
  date: new Date(FIXTURE_NOW.getTime() - 2 * 24 * 60 * 60 * 1000),
  pending: true,
});

// Removed — must be skipped.
export const txRemoved: Transaction = buildTransaction({
  id: 'tx_removed',
  amountCents: 60_000n,
  date: new Date(FIXTURE_NOW.getTime() - 5 * 24 * 60 * 60 * 1000),
  removedAt: new Date('2026-05-05T00:00:00Z'),
});

// At-now boundary -> excluded (window end is exclusive).
export const txAtNowBoundary: Transaction = buildTransaction({
  id: 'tx_now',
  amountCents: 99_999n,
  date: new Date(FIXTURE_NOW.getTime()),
});

// Zero-amount tx (inside window) -> doesn't contribute to either bucket.
export const txZeroAmount: Transaction = buildTransaction({
  id: 'tx_zero',
  amountCents: 0n,
  date: new Date(FIXTURE_NOW.getTime() - 3 * 24 * 60 * 60 * 1000),
});

export const window30DayMixed: ReadonlyArray<Transaction> = [
  txAtStartBoundary,
  txOneMsBeforeStart,
  txOutflow200,
  txInflow5000,
  txPending,
  txRemoved,
  txAtNowBoundary,
  txZeroAmount,
];
