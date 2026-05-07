// Tests for `lib/compute/cash-only.ts`.

import { describe, it, expect } from 'vitest';

import { cashOnly } from '../../../lib/compute/cash-only.js';
import { buildAccount } from '../../fixtures/compute/builders.js';
import { allZeroAccounts } from '../../fixtures/compute/all-zero.js';
import { emptyAccounts } from '../../fixtures/compute/empty.js';
import { mixedPositiveAccounts } from '../../fixtures/compute/mixed-positive.js';
import { singleCheckingAccounts } from '../../fixtures/compute/single-checking.js';

describe('cashOnly — empty', () => {
  it('returns 0n for empty input', () => {
    expect(cashOnly({ accounts: emptyAccounts })).toBe(0n);
  });
});

describe('cashOnly — single depository', () => {
  it('returns the balance', () => {
    expect(cashOnly({ accounts: singleCheckingAccounts })).toBe(500_000n);
  });
});

describe('cashOnly — mixed', () => {
  it('sums depository accounts only', () => {
    // checking 1_000_000 + savings 2_500_000 = 3_500_000
    // investment + credit are excluded
    expect(cashOnly({ accounts: mixedPositiveAccounts })).toBe(3_500_000n);
  });

  it('ignores investment accounts', () => {
    const inv = buildAccount({ type: 'investment', currentBalanceCents: 9_999_999n });
    expect(cashOnly({ accounts: [inv] })).toBe(0n);
  });

  it('ignores credit accounts', () => {
    const cc = buildAccount({ type: 'credit', currentBalanceCents: 100_000n });
    expect(cashOnly({ accounts: [cc] })).toBe(0n);
  });

  it('ignores loan accounts', () => {
    const loan = buildAccount({ type: 'loan', currentBalanceCents: 100_000n });
    expect(cashOnly({ accounts: [loan] })).toBe(0n);
  });

  it('ignores other accounts', () => {
    const other = buildAccount({ type: 'other', currentBalanceCents: 100_000n });
    expect(cashOnly({ accounts: [other] })).toBe(0n);
  });
});

describe('cashOnly — zero & negative balances', () => {
  it('ignores zero-balance depository accounts', () => {
    expect(cashOnly({ accounts: allZeroAccounts })).toBe(0n);
  });

  it('ignores negative-balance depository accounts (overdraft)', () => {
    const overdraft = buildAccount({
      id: 'acct_od',
      type: 'depository',
      currentBalanceCents: -100_000n,
    });
    expect(cashOnly({ accounts: [overdraft] })).toBe(0n);
  });
});

describe('cashOnly — null balances', () => {
  it('ignores null currentBalanceCents', () => {
    const nullBal = buildAccount({
      id: 'acct_nb',
      type: 'depository',
      currentBalanceCents: null,
    });
    expect(cashOnly({ accounts: [nullBal] })).toBe(0n);
  });
});

describe('cashOnly — closed accounts', () => {
  it('excludes closed depository accounts', () => {
    const open = buildAccount({ id: 'acct_o', type: 'depository', currentBalanceCents: 100n });
    const closed = buildAccount({
      id: 'acct_c',
      type: 'depository',
      currentBalanceCents: 999_999_999n,
      closedAt: new Date('2024-01-01T00:00:00Z'),
    });
    expect(cashOnly({ accounts: [open, closed] })).toBe(100n);
  });
});
