// Tests for `lib/compute/index.ts` — factory wiring.

import { describe, it, expect } from 'vitest';

import {
  billionProgress,
  cashOnly,
  centsAbs,
  centsToDisplay,
  createComputeService,
  monthNet,
  netWorth,
  toCents,
} from '../../../lib/compute/index.js';
import type { Cents } from '../../../lib/types/domain.js';
import { emptyAccounts, emptyTransactions } from '../../fixtures/compute/empty.js';
import { mixedPositiveAccounts } from '../../fixtures/compute/mixed-positive.js';
import { FIXTURE_NOW } from '../../fixtures/compute/transactions-30day.js';

describe('createComputeService', () => {
  it('returns a service that delegates to the pure functions', () => {
    const svc = createComputeService();
    const direct = netWorth({ accounts: mixedPositiveAccounts });
    const viaSvc = svc.netWorth({ accounts: mixedPositiveAccounts });
    expect(viaSvc.assetsCents).toBe(direct.assetsCents);
    expect(viaSvc.liabilitiesCents).toBe(direct.liabilitiesCents);
    expect(viaSvc.netWorthCents).toBe(direct.netWorthCents);
    expect(viaSvc.cashCents).toBe(direct.cashCents);
  });

  it('exposes cashOnly', () => {
    const svc = createComputeService();
    expect(svc.cashOnly({ accounts: emptyAccounts })).toBe(0n);
    // cross-check direct call
    expect(cashOnly({ accounts: emptyAccounts })).toBe(0n);
  });

  it('exposes monthNet', () => {
    const svc = createComputeService();
    const r = svc.monthNet({ transactions: emptyTransactions, now: FIXTURE_NOW });
    expect(r.netCents).toBe(0n);
    // cross-check direct call
    expect(monthNet({ transactions: emptyTransactions, now: FIXTURE_NOW }).netCents).toBe(0n);
  });

  it('exposes billionProgress', () => {
    const svc = createComputeService();
    const r = svc.billionProgress({ netWorthCents: 50_000_000_000n as Cents });
    expect(r.progress).toBeCloseTo(0.5, 4);
    // cross-check direct call
    expect(billionProgress({ netWorthCents: 50_000_000_000n as Cents }).progress).toBeCloseTo(
      0.5,
      4,
    );
  });
});

describe('currency exports', () => {
  it('re-exports toCents / centsToDisplay / centsAbs', () => {
    expect(toCents('1.50')).toBe(150n);
    expect(centsToDisplay(150n as Cents)).toBe('$1.50');
    expect(centsAbs(-150n as Cents)).toBe(150n);
  });
});
