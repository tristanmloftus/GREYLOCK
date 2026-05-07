// Greylock — compute fixture builders
// =============================================================================
// Used only by `tests/unit/compute/**`. Production code MUST NOT import this.
//
// These builders produce minimally-populated `Account` and `Transaction`
// objects for use in compute fixtures. Compute functions only read a handful
// of fields, so we fill the rest with deterministic placeholders.
// =============================================================================

import type {
  Account,
  AccountId,
  Cents,
  IsoCurrencyCode,
  ItemId,
  PlaidAccountId,
  PlaidTransactionId,
  Transaction,
  TransactionId,
  UserId,
} from '../../../lib/types/domain.js';
import {
  AccountId as AccountIdCtor,
  ItemId as ItemIdCtor,
  TransactionId as TransactionIdCtor,
  UserId as UserIdCtor,
} from '../../../lib/types/domain.js';

const ZERO_DATE = new Date('2026-01-01T00:00:00.000Z');
const USD: IsoCurrencyCode = 'USD';

export interface AccountOverrides {
  id?: string;
  name?: string;
  type?: Account['type'];
  subtype?: string | null;
  currentBalanceCents?: Cents | null;
  availableBalanceCents?: Cents | null;
  limitCents?: Cents | null;
  closedAt?: Date | null;
  domain?: Account['domain'];
  userId?: string | null;
  itemId?: string;
  plaidAccountId?: string;
  mask?: string | null;
  officialName?: string | null;
}

export const buildAccount = (overrides: AccountOverrides = {}): Account => {
  const id = AccountIdCtor(overrides.id ?? 'acct_1');
  const itemId = ItemIdCtor(overrides.itemId ?? 'item_1');
  const userId =
    overrides.userId === null
      ? null
      : overrides.userId !== undefined
        ? UserIdCtor(overrides.userId)
        : UserIdCtor('usr_test');

  return {
    id,
    itemId,
    domain: overrides.domain ?? 'personal',
    userId,
    plaidAccountId: (overrides.plaidAccountId ?? 'plaid_acct_1') as PlaidAccountId,
    name: overrides.name ?? 'Test Account',
    officialName: overrides.officialName ?? null,
    mask: overrides.mask ?? '0000',
    type: overrides.type ?? 'depository',
    subtype: overrides.subtype ?? null,
    isoCurrencyCode: USD,
    currentBalanceCents:
      overrides.currentBalanceCents === undefined ? 0n : overrides.currentBalanceCents,
    availableBalanceCents:
      overrides.availableBalanceCents === undefined ? null : overrides.availableBalanceCents,
    limitCents: overrides.limitCents === undefined ? null : overrides.limitCents,
    balanceUpdatedAt: ZERO_DATE,
    createdAt: ZERO_DATE,
    updatedAt: ZERO_DATE,
    closedAt: overrides.closedAt === undefined ? null : overrides.closedAt,
  };
};

export interface TransactionOverrides {
  id?: string;
  amountCents?: Cents;
  date?: Date;
  pending?: boolean;
  removedAt?: Date | null;
  domain?: Transaction['domain'];
  userId?: string | null;
  itemId?: string;
  accountId?: string;
  plaidTransactionId?: string;
  name?: string;
  merchantName?: string | null;
}

export const buildTransaction = (overrides: TransactionOverrides = {}): Transaction => {
  const id: TransactionId = TransactionIdCtor(overrides.id ?? 'tx_1');
  const itemId: ItemId = ItemIdCtor(overrides.itemId ?? 'item_1');
  const accountId: AccountId = AccountIdCtor(overrides.accountId ?? 'acct_1');
  const userId: UserId | null =
    overrides.userId === null
      ? null
      : overrides.userId !== undefined
        ? UserIdCtor(overrides.userId)
        : UserIdCtor('usr_test');

  return {
    id,
    itemId,
    accountId,
    domain: overrides.domain ?? 'personal',
    userId,
    plaidTransactionId: (overrides.plaidTransactionId ?? 'plaid_tx_1') as PlaidTransactionId,
    amountCents: overrides.amountCents === undefined ? 0n : overrides.amountCents,
    isoCurrencyCode: USD,
    date: overrides.date ?? ZERO_DATE,
    authorizedDate: null,
    name: overrides.name ?? 'Test Transaction',
    merchantName: overrides.merchantName ?? null,
    pending: overrides.pending ?? false,
    category: null,
    categoryDetailed: null,
    createdAt: ZERO_DATE,
    updatedAt: ZERO_DATE,
    removedAt: overrides.removedAt === undefined ? null : overrides.removedAt,
  };
};
