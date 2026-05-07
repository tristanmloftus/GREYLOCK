// Tests for `lib/compute/month-net.ts`.

import { describe, it, expect } from 'vitest';

import { monthNet } from '../../../lib/compute/month-net.js';
import { buildTransaction } from '../../fixtures/compute/builders.js';
import { emptyTransactions } from '../../fixtures/compute/empty.js';
import {
  FIXTURE_NOW,
  FIXTURE_WINDOW_START,
  txAtNowBoundary,
  txAtStartBoundary,
  txInflow5000,
  txOneMsBeforeStart,
  txOutflow200,
  txPending,
  txRemoved,
  txZeroAmount,
  window30DayMixed,
} from '../../fixtures/compute/transactions-30day.js';

describe('monthNet — empty', () => {
  it('returns zeros and the correct window', () => {
    const result = monthNet({ transactions: emptyTransactions, now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(0n);
    expect(result.outflowCents).toBe(0n);
    expect(result.netCents).toBe(0n);
    expect(result.windowStart.getTime()).toBe(FIXTURE_WINDOW_START.getTime());
    expect(result.windowEnd.getTime()).toBe(FIXTURE_NOW.getTime());
  });
});

describe('monthNet — pure inflow', () => {
  it('produces inflow only', () => {
    const result = monthNet({ transactions: [txInflow5000], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(500_000n);
    expect(result.outflowCents).toBe(0n);
    expect(result.netCents).toBe(500_000n);
  });
});

describe('monthNet — pure outflow', () => {
  it('produces outflow only', () => {
    const result = monthNet({ transactions: [txOutflow200], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(0n);
    expect(result.outflowCents).toBe(20_000n);
    expect(result.netCents).toBe(-20_000n);
  });
});

describe('monthNet — mixed window', () => {
  it('correctly classifies the multi-fixture set', () => {
    const result = monthNet({ transactions: window30DayMixed, now: FIXTURE_NOW });
    // Included:
    //   txAtStartBoundary       1_000n outflow
    //   txOutflow200           20_000n outflow
    //   txInflow5000         -500_000n -> 500_000n inflow
    //   txZeroAmount               0n  (skipped — no contribution)
    // Excluded:
    //   txOneMsBeforeStart  (before window)
    //   txPending            (pending)
    //   txRemoved            (removedAt set)
    //   txAtNowBoundary      (>= now)
    expect(result.inflowCents).toBe(500_000n);
    expect(result.outflowCents).toBe(21_000n);
    expect(result.netCents).toBe(479_000n);
  });
});

describe('monthNet — window boundaries', () => {
  it('includes a transaction exactly at the start of the window', () => {
    const result = monthNet({ transactions: [txAtStartBoundary], now: FIXTURE_NOW });
    expect(result.outflowCents).toBe(1_000n);
  });

  it('excludes a transaction one ms before the window start', () => {
    const result = monthNet({ transactions: [txOneMsBeforeStart], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(0n);
    expect(result.outflowCents).toBe(0n);
  });

  it('excludes a transaction exactly at `now` (end exclusive)', () => {
    const result = monthNet({ transactions: [txAtNowBoundary], now: FIXTURE_NOW });
    expect(result.outflowCents).toBe(0n);
    expect(result.netCents).toBe(0n);
  });
});

describe('monthNet — pending and removed', () => {
  it('skips pending transactions', () => {
    const result = monthNet({ transactions: [txPending], now: FIXTURE_NOW });
    expect(result.outflowCents).toBe(0n);
  });

  it('skips removed transactions', () => {
    const result = monthNet({ transactions: [txRemoved], now: FIXTURE_NOW });
    expect(result.outflowCents).toBe(0n);
  });
});

describe('monthNet — zero amounts', () => {
  it('does not contribute to either bucket', () => {
    const result = monthNet({ transactions: [txZeroAmount], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(0n);
    expect(result.outflowCents).toBe(0n);
    expect(result.netCents).toBe(0n);
  });
});

describe('monthNet — sign convention round-trip', () => {
  it('keeps inflow magnitude when amount is negative', () => {
    const tx = buildTransaction({
      id: 'tx_round',
      amountCents: -123_456n,
      date: new Date(FIXTURE_NOW.getTime() - 24 * 60 * 60 * 1000),
    });
    const result = monthNet({ transactions: [tx], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(123_456n);
    expect(result.netCents).toBe(123_456n);
  });

  it('keeps outflow magnitude when amount is positive', () => {
    const tx = buildTransaction({
      id: 'tx_round_out',
      amountCents: 654_321n,
      date: new Date(FIXTURE_NOW.getTime() - 24 * 60 * 60 * 1000),
    });
    const result = monthNet({ transactions: [tx], now: FIXTURE_NOW });
    expect(result.outflowCents).toBe(654_321n);
    expect(result.netCents).toBe(-654_321n);
  });

  it('handles paired inflow + outflow', () => {
    const inflow = buildTransaction({
      id: 'tx_in',
      amountCents: -100_000n,
      date: new Date(FIXTURE_NOW.getTime() - 5 * 24 * 60 * 60 * 1000),
    });
    const outflow = buildTransaction({
      id: 'tx_out',
      amountCents: 30_000n,
      date: new Date(FIXTURE_NOW.getTime() - 4 * 24 * 60 * 60 * 1000),
    });
    const result = monthNet({ transactions: [inflow, outflow], now: FIXTURE_NOW });
    expect(result.inflowCents).toBe(100_000n);
    expect(result.outflowCents).toBe(30_000n);
    expect(result.netCents).toBe(70_000n);
  });
});

describe('monthNet — input not mutated', () => {
  it('does not mutate the supplied array', () => {
    const snapshot = window30DayMixed.slice();
    monthNet({ transactions: window30DayMixed, now: FIXTURE_NOW });
    expect(window30DayMixed.length).toBe(snapshot.length);
    for (let i = 0; i < snapshot.length; i++) {
      expect(window30DayMixed[i]).toBe(snapshot[i]);
    }
  });
});
