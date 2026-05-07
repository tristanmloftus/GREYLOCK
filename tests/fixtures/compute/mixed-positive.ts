// Fixture: positive net worth with a mix of assets and liabilities.
//
//   checking          +  10,000.00  (depository -> asset, cash)
//   savings           +  25,000.00  (depository -> asset, cash)
//   investment        + 150,000.00  (investment -> asset)
//   credit card       -   3,250.00  (credit -> liability)
//
//   assets      = 185,000.00
//   liabilities =   3,250.00
//   netWorth    = 181,750.00
//   cash        =  35,000.00

import type { Account } from '../../../lib/types/domain.js';

import { buildAccount } from './builders.js';

export const mixedCheckingAccount: Account = buildAccount({
  id: 'acct_chk',
  name: 'Checking',
  type: 'depository',
  subtype: 'checking',
  currentBalanceCents: 1_000_000n,
});

export const mixedSavingsAccount: Account = buildAccount({
  id: 'acct_sav',
  name: 'Savings',
  type: 'depository',
  subtype: 'savings',
  currentBalanceCents: 2_500_000n,
});

export const mixedInvestmentAccount: Account = buildAccount({
  id: 'acct_inv',
  name: 'Brokerage',
  type: 'investment',
  subtype: 'brokerage',
  currentBalanceCents: 15_000_000n,
});

export const mixedCreditAccount: Account = buildAccount({
  id: 'acct_cc',
  name: 'Visa',
  type: 'credit',
  subtype: 'credit card',
  currentBalanceCents: 325_000n,
});

export const mixedPositiveAccounts: ReadonlyArray<Account> = [
  mixedCheckingAccount,
  mixedSavingsAccount,
  mixedInvestmentAccount,
  mixedCreditAccount,
];
