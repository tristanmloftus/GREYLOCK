// Tests for `lib/compute/net-worth.ts`.

import { describe, it, expect } from 'vitest';

import { netWorth } from '../../../lib/compute/net-worth.js';
import { buildAccount } from '../../fixtures/compute/builders.js';
import { allZeroAccounts } from '../../fixtures/compute/all-zero.js';
import { emptyAccounts } from '../../fixtures/compute/empty.js';
import {
  mixedCheckingAccount,
  mixedCreditAccount,
  mixedInvestmentAccount,
  mixedPositiveAccounts,
  mixedSavingsAccount,
} from '../../fixtures/compute/mixed-positive.js';
import { negativeNwAccounts } from '../../fixtures/compute/negative-nw.js';
import { singleCheckingAccounts } from '../../fixtures/compute/single-checking.js';

describe('netWorth — empty input', () => {
  it('returns all zeros and empty breakdown', () => {
    const result = netWorth({ accounts: emptyAccounts });
    expect(result.assetsCents).toBe(0n);
    expect(result.liabilitiesCents).toBe(0n);
    expect(result.netWorthCents).toBe(0n);
    expect(result.cashCents).toBe(0n);
    expect(result.breakdown).toEqual([]);
  });
});

describe('netWorth — single depository account', () => {
  it('treats checking as asset and as cash', () => {
    const result = netWorth({ accounts: singleCheckingAccounts });
    expect(result.assetsCents).toBe(500_000n);
    expect(result.liabilitiesCents).toBe(0n);
    expect(result.netWorthCents).toBe(500_000n);
    expect(result.cashCents).toBe(500_000n);
    expect(result.breakdown).toHaveLength(1);
    expect(result.breakdown[0]?.contribution).toBe('asset');
    expect(result.breakdown[0]?.balanceCents).toBe(500_000n);
  });
});

describe('netWorth — mixed positive', () => {
  it('sums assets and liabilities correctly', () => {
    const result = netWorth({ accounts: mixedPositiveAccounts });
    // assets = 1_000_000 + 2_500_000 + 15_000_000 = 18_500_000
    expect(result.assetsCents).toBe(18_500_000n);
    expect(result.liabilitiesCents).toBe(325_000n);
    expect(result.netWorthCents).toBe(18_175_000n);
    // cash = checking + savings (depository only)
    expect(result.cashCents).toBe(3_500_000n);
  });

  it('includes one breakdown line per account', () => {
    const result = netWorth({ accounts: mixedPositiveAccounts });
    expect(result.breakdown).toHaveLength(4);
    const byId = new Map(result.breakdown.map((b) => [b.accountId, b]));
    expect(byId.get(mixedCheckingAccount.id)?.contribution).toBe('asset');
    expect(byId.get(mixedSavingsAccount.id)?.contribution).toBe('asset');
    expect(byId.get(mixedInvestmentAccount.id)?.contribution).toBe('asset');
    expect(byId.get(mixedCreditAccount.id)?.contribution).toBe('liability');
  });
});

describe('netWorth — all-zero balances', () => {
  it('yields zero NW but still lists every account', () => {
    const result = netWorth({ accounts: allZeroAccounts });
    expect(result.assetsCents).toBe(0n);
    expect(result.liabilitiesCents).toBe(0n);
    expect(result.netWorthCents).toBe(0n);
    expect(result.cashCents).toBe(0n);
    expect(result.breakdown).toHaveLength(allZeroAccounts.length);
  });
});

describe('netWorth — negative net worth', () => {
  it('handles liabilities exceeding assets', () => {
    const result = netWorth({ accounts: negativeNwAccounts });
    // assets: 50_000; liabilities: 250_000 + 1_000_000 = 1_250_000
    expect(result.assetsCents).toBe(50_000n);
    expect(result.liabilitiesCents).toBe(1_250_000n);
    expect(result.netWorthCents).toBe(-1_200_000n);
    expect(result.cashCents).toBe(50_000n);
  });
});

describe('netWorth — closed accounts', () => {
  it('excludes accounts with closedAt set', () => {
    const open = buildAccount({
      id: 'acct_open',
      type: 'depository',
      currentBalanceCents: 100_000n,
    });
    const closed = buildAccount({
      id: 'acct_closed',
      type: 'depository',
      currentBalanceCents: 999_999_999n,
      closedAt: new Date('2024-01-01T00:00:00Z'),
    });
    const result = netWorth({ accounts: [open, closed] });
    expect(result.assetsCents).toBe(100_000n);
    expect(result.cashCents).toBe(100_000n);
    expect(result.breakdown).toHaveLength(1);
    expect(result.breakdown[0]?.accountId).toBe(open.id);
  });
});

describe('netWorth — null balances', () => {
  it('treats null currentBalanceCents as 0', () => {
    const a = buildAccount({
      id: 'acct_null',
      type: 'depository',
      currentBalanceCents: null,
    });
    const result = netWorth({ accounts: [a] });
    expect(result.assetsCents).toBe(0n);
    expect(result.cashCents).toBe(0n);
    expect(result.breakdown[0]?.balanceCents).toBe(0n);
    expect(result.breakdown[0]?.contribution).toBe('asset');
  });
});

describe('netWorth — type contributions', () => {
  it('treats `loan` as a liability', () => {
    const loan = buildAccount({ id: 'acct_loan', type: 'loan', currentBalanceCents: 75_000n });
    const result = netWorth({ accounts: [loan] });
    expect(result.liabilitiesCents).toBe(75_000n);
    expect(result.assetsCents).toBe(0n);
    expect(result.netWorthCents).toBe(-75_000n);
    expect(result.cashCents).toBe(0n);
    expect(result.breakdown[0]?.contribution).toBe('liability');
  });

  it('treats `investment` as an asset but NOT cash', () => {
    const inv = buildAccount({ id: 'acct_inv', type: 'investment', currentBalanceCents: 1_000_000n });
    const result = netWorth({ accounts: [inv] });
    expect(result.assetsCents).toBe(1_000_000n);
    expect(result.cashCents).toBe(0n);
  });

  it('treats `other` as an asset (documented v0.1 default)', () => {
    const other = buildAccount({ id: 'acct_oth', type: 'other', currentBalanceCents: 12_345n });
    const result = netWorth({ accounts: [other] });
    expect(result.assetsCents).toBe(12_345n);
    expect(result.cashCents).toBe(0n);
    expect(result.breakdown[0]?.contribution).toBe('asset');
  });
});

describe('netWorth — input is not mutated', () => {
  it('does not modify the supplied account array', () => {
    const original = [...mixedPositiveAccounts];
    netWorth({ accounts: mixedPositiveAccounts });
    expect(mixedPositiveAccounts.length).toBe(original.length);
    for (let i = 0; i < original.length; i++) {
      expect(mixedPositiveAccounts[i]).toBe(original[i]);
    }
  });
});
