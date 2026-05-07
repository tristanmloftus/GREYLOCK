// Fixture: empty inputs.
//
// Compute functions with no data must return a defined zero state, not throw
// and not return undefined. This is the bedrock fixture every other test
// implicitly extends.

import type { Account, Transaction } from '../../../lib/types/domain.js';

export const emptyAccounts: ReadonlyArray<Account> = [];
export const emptyTransactions: ReadonlyArray<Transaction> = [];
