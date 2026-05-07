// Fixture: a single depository (checking) account with $5,000.

import type { Account } from '../../../lib/types/domain.js';

import { buildAccount } from './builders.js';

export const singleCheckingAccount: Account = buildAccount({
  id: 'acct_chk_1',
  name: 'Chase Checking',
  type: 'depository',
  subtype: 'checking',
  currentBalanceCents: 500_000n, // $5,000.00
});

export const singleCheckingAccounts: ReadonlyArray<Account> = [singleCheckingAccount];
