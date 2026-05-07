// Greylock — Plaid mapper unit tests
// =============================================================================
// Cents conversion edge cases (round-trip, sign preservation, .5 rounding,
// negative, zero, large magnitudes), account-type mapping, transaction
// mapping with accountId resolution.
// =============================================================================

import { describe, it, expect } from 'vitest';

import { AccountId, ItemId, UserId } from '../../../lib/types/domain.js';
import type { PlaidAccountId } from '../../../lib/types/domain.js';
import {
  dollarsToCents,
  dollarsToCentsRequired,
  mapPlaidAccount,
  mapPlaidTransaction,
  mapRemovedTransactionId,
} from '../../../lib/plaid/mappers.js';
import type { AccountBase, RemovedTransaction, Transaction as PlaidTx } from 'plaid';

describe('mappers — dollarsToCents', () => {
  it('returns null for null/undefined', () => {
    expect(dollarsToCents(null)).toBe(null);
    expect(dollarsToCents(undefined)).toBe(null);
  });

  it('zero → 0n', () => {
    expect(dollarsToCents(0)).toBe(0n);
  });

  it('positive integer dollars', () => {
    expect(dollarsToCents(12)).toBe(1200n);
    expect(dollarsToCents(1)).toBe(100n);
  });

  it('positive cents (.34)', () => {
    expect(dollarsToCents(12.34)).toBe(1234n);
  });

  it('negative dollars', () => {
    expect(dollarsToCents(-12.34)).toBe(-1234n);
    expect(dollarsToCents(-0.05)).toBe(-5n);
  });

  it('exact .5 cents — half-away-from-zero', () => {
    // 0.005 dollars = 0.5 cents. Half-away-from-zero rounds to 1 cent positive.
    expect(dollarsToCents(0.005)).toBe(1n);
    expect(dollarsToCents(-0.005)).toBe(-1n);
  });

  it('half-cents that already exceed .5', () => {
    expect(dollarsToCents(0.006)).toBe(1n);
    expect(dollarsToCents(0.014)).toBe(1n); // 1.4 cents → rounds to 1
    expect(dollarsToCents(0.015)).toBe(2n); // 1.5 cents → rounds to 2
  });

  it('large magnitude', () => {
    expect(dollarsToCents(1_000_000)).toBe(100_000_000n);
    expect(dollarsToCents(-1_000_000)).toBe(-100_000_000n);
  });

  it('throws on non-finite input', () => {
    expect(() => dollarsToCents(Number.NaN)).toThrow(/non-finite/u);
    expect(() => dollarsToCents(Number.POSITIVE_INFINITY)).toThrow(/non-finite/u);
    expect(() => dollarsToCents(Number.NEGATIVE_INFINITY)).toThrow(/non-finite/u);
  });

  it('throws on out-of-range magnitude', () => {
    expect(() => dollarsToCents(2e15)).toThrow(/magnitude/u);
    expect(() => dollarsToCents(-2e15)).toThrow(/magnitude/u);
  });

  it('round-trip: cents → dollars → cents (within precision)', () => {
    // For amounts representable in float without drift.
    const samples = [0, 100, -100, 12345, -987.65, 0.5, -0.5, 1234567.89];
    for (const s of samples) {
      const cents = dollarsToCentsRequired(s);
      const back = Number(cents) / 100;
      // Reconvert and assert exact match.
      expect(dollarsToCentsRequired(back)).toBe(cents);
    }
  });
});

describe('mappers — dollarsToCentsRequired', () => {
  it('throws on null', () => {
    expect(() => dollarsToCentsRequired(null)).toThrow(/required/u);
    expect(() => dollarsToCentsRequired(undefined)).toThrow(/required/u);
  });
  it('passes through valid', () => {
    expect(dollarsToCentsRequired(1.23)).toBe(123n);
  });
});

// -----------------------------------------------------------------------------
// Account mapper
// -----------------------------------------------------------------------------

function fakeAccount(over: Partial<AccountBase> = {}): AccountBase {
  return {
    account_id: 'plaid_acc_1',
    balances: {
      current: 1234.56,
      available: 1000.0,
      limit: null,
      iso_currency_code: 'USD',
      unofficial_currency_code: null,
      last_updated_datetime: null,
    },
    mask: '1234',
    name: 'Checking',
    official_name: 'Premium Checking',
    type: 'depository' as never,
    subtype: 'checking' as never,
    ...over,
  };
}

describe('mappers — mapPlaidAccount', () => {
  it('maps standard depository USD account', () => {
    const out = mapPlaidAccount(fakeAccount());
    expect(out.plaidAccountId).toBe('plaid_acc_1');
    expect(out.name).toBe('Checking');
    expect(out.type).toBe('depository');
    expect(out.currentBalanceCents).toBe(123456n);
    expect(out.availableBalanceCents).toBe(100000n);
    expect(out.limitCents).toBe(null);
    expect(out.isoCurrencyCode).toBe('USD');
  });

  it('maps brokerage to investment (legacy alias)', () => {
    const out = mapPlaidAccount(fakeAccount({ type: 'brokerage' as never }));
    expect(out.type).toBe('investment');
  });

  it('maps unknown account type to other', () => {
    const out = mapPlaidAccount(fakeAccount({ type: 'unknown_type' as never }));
    expect(out.type).toBe('other');
  });

  it('rejects non-USD account currency at v0.1', () => {
    expect(() =>
      mapPlaidAccount(
        fakeAccount({
          balances: {
            current: 100,
            available: null,
            limit: null,
            iso_currency_code: 'EUR',
            unofficial_currency_code: null,
            last_updated_datetime: null,
          },
        }),
      ),
    ).toThrow(/non-USD/u);
  });

  it('handles null balances', () => {
    const out = mapPlaidAccount(
      fakeAccount({
        balances: {
          current: null,
          available: null,
          limit: null,
          iso_currency_code: null,
          unofficial_currency_code: null,
          last_updated_datetime: null,
        },
      }),
    );
    expect(out.currentBalanceCents).toBe(null);
    expect(out.availableBalanceCents).toBe(null);
    expect(out.limitCents).toBe(null);
  });
});

// -----------------------------------------------------------------------------
// Transaction mapper
// -----------------------------------------------------------------------------

function fakeTx(over: Partial<PlaidTx> = {}): PlaidTx {
  return {
    account_id: 'plaid_acc_1',
    amount: 12.34,
    iso_currency_code: 'USD',
    unofficial_currency_code: null,
    category: ['Shops', 'Groceries'],
    category_id: '19046000',
    date: '2025-01-15',
    location: {
      address: null,
      city: null,
      country: null,
      lat: null,
      lon: null,
      postal_code: null,
      region: null,
      store_number: null,
    },
    name: 'Whole Foods',
    merchant_name: 'Whole Foods Market',
    payment_meta: {
      by_order_of: null,
      payee: null,
      payer: null,
      payment_method: null,
      payment_processor: null,
      ppd_id: null,
      reason: null,
      reference_number: null,
    },
    pending: false,
    pending_transaction_id: null,
    account_owner: null,
    transaction_id: 'plaid_tx_1',
    payment_channel: 'in store' as never,
    transaction_code: null,
    authorized_date: '2025-01-14',
    authorized_datetime: null,
    datetime: null,
    ...over,
  };
}

describe('mappers — mapPlaidTransaction', () => {
  it('maps standard transaction with sign preserved (positive = outflow)', () => {
    const tx = fakeTx();
    const out = mapPlaidTransaction(
      {
        itemId: ItemId('item_1'),
        accountIdResolver: () => AccountId('acc_1'),
        domain: 'personal',
        userId: UserId('user_1'),
      },
      tx,
    );
    expect(out).not.toBe(null);
    expect(out!.amountCents).toBe(1234n); // positive — outflow
    expect(out!.name).toBe('Whole Foods');
    expect(out!.merchantName).toBe('Whole Foods Market');
    expect(out!.category).toBe('Shops');
    expect(out!.categoryDetailed).toBe('Groceries');
    expect(out!.date.toISOString()).toBe('2025-01-15T00:00:00.000Z');
    expect(out!.authorizedDate?.toISOString()).toBe('2025-01-14T00:00:00.000Z');
    expect(out!.pending).toBe(false);
    expect(out!.itemId).toBe('item_1');
    expect(out!.accountId).toBe('acc_1');
  });

  it('preserves negative amount (inflow)', () => {
    const tx = fakeTx({ amount: -50.0 });
    const out = mapPlaidTransaction(
      {
        itemId: ItemId('item_1'),
        accountIdResolver: () => AccountId('acc_1'),
        domain: 'personal',
        userId: UserId('user_1'),
      },
      tx,
    );
    expect(out!.amountCents).toBe(-5000n);
  });

  it('returns null when accountId can not be resolved', () => {
    const tx = fakeTx();
    const out = mapPlaidTransaction(
      {
        itemId: ItemId('item_1'),
        accountIdResolver: () => null,
        domain: 'personal',
        userId: UserId('user_1'),
      },
      tx,
    );
    expect(out).toBe(null);
  });

  it('rejects non-USD transaction currency', () => {
    const tx = fakeTx({ iso_currency_code: 'EUR' });
    expect(() =>
      mapPlaidTransaction(
        {
          itemId: ItemId('item_1'),
          accountIdResolver: () => AccountId('acc_1'),
          domain: 'personal',
          userId: UserId('user_1'),
        },
        tx,
      ),
    ).toThrow(/non-USD/u);
  });

  it('throws on malformed date', () => {
    const tx = fakeTx({ date: 'not-a-date' });
    expect(() =>
      mapPlaidTransaction(
        {
          itemId: ItemId('item_1'),
          accountIdResolver: () => AccountId('acc_1'),
          domain: 'personal',
          userId: UserId('user_1'),
        },
        tx,
      ),
    ).toThrow(/malformed Plaid date/u);
  });

  it('handles missing authorized_date', () => {
    const tx = fakeTx({ authorized_date: null });
    const out = mapPlaidTransaction(
      {
        itemId: ItemId('item_1'),
        accountIdResolver: () => AccountId('acc_1'),
        domain: 'pcc',
        userId: null,
      },
      tx,
    );
    expect(out!.authorizedDate).toBe(null);
    expect(out!.userId).toBe(null);
    expect(out!.domain).toBe('pcc');
  });
});

describe('mappers — mapRemovedTransactionId', () => {
  it('extracts plaid transaction id', () => {
    const r: RemovedTransaction = { transaction_id: 'plaid_tx_zorp', account_id: 'plaid_acc_1' };
    expect(mapRemovedTransactionId(r)).toBe('plaid_tx_zorp');
  });
});

// Local resolver helper for type narrowness; ensures `PlaidAccountId` brand
// flow stays consistent across the test suite.
void (null as unknown as PlaidAccountId);
