// Fixture: accounts present but all balances are zero.
//
// Distinguishes "I have accounts" from "I have nothing" — both must produce
// 0 net worth, but the breakdown should still list the accounts.

import type { Account } from '../../../lib/types/domain.js';

import { buildAccount } from './builders.js';

export const allZeroAccounts: ReadonlyArray<Account> = [
  buildAccount({ id: 'acct_z1', name: 'Empty Checking', type: 'depository', currentBalanceCents: 0n }),
  buildAccount({ id: 'acct_z2', name: 'Empty Savings', type: 'depository', currentBalanceCents: 0n }),
  buildAccount({ id: 'acct_z3', name: 'Paid-Off Card', type: 'credit', currentBalanceCents: 0n }),
];
