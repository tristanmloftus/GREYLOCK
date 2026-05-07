// Fixture: liabilities exceed assets => net worth is negative.
//
//   checking          +    500.00   (asset, cash)
//   credit card 1     -  2,500.00   (liability)
//   loan              - 10,000.00   (liability)
//
//   assets      =    500.00
//   liabilities = 12,500.00
//   netWorth    = -12,000.00

import type { Account } from '../../../lib/types/domain.js';

import { buildAccount } from './builders.js';

export const negativeCheckingAccount: Account = buildAccount({
  id: 'acct_chk_neg',
  name: 'Thin Checking',
  type: 'depository',
  currentBalanceCents: 50_000n,
});

export const negativeCreditAccount: Account = buildAccount({
  id: 'acct_cc_neg',
  name: 'Maxed Card',
  type: 'credit',
  currentBalanceCents: 250_000n,
});

export const negativeLoanAccount: Account = buildAccount({
  id: 'acct_ln_neg',
  name: 'Auto Loan',
  type: 'loan',
  currentBalanceCents: 1_000_000n,
});

export const negativeNwAccounts: ReadonlyArray<Account> = [
  negativeCheckingAccount,
  negativeCreditAccount,
  negativeLoanAccount,
];
