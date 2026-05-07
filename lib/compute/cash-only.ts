// Greylock — cash-only compute (pure)
// =============================================================================
// AGENT-COMPUTE owns this file. No I/O.
//
// `cashOnly` returns the sum of `depository` accounts whose balance is
// strictly positive. Closed accounts (`closedAt !== null`) and null balances
// are excluded. Investments and credit accounts NEVER count as cash.
// =============================================================================

import type { Account, Cents } from '../types/domain.js';

export const cashOnly = (input: { readonly accounts: ReadonlyArray<Account> }): Cents => {
  let total = 0n;
  for (const a of input.accounts) {
    if (a.closedAt !== null) {
      continue;
    }
    if (a.type !== 'depository') {
      continue;
    }
    const balance = a.currentBalanceCents;
    if (balance === null) {
      continue;
    }
    if (balance <= 0n) {
      continue;
    }
    total += balance;
  }
  return total as Cents;
};
