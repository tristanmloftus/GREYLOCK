// Greylock — net-worth compute (pure)
// =============================================================================
// AGENT-COMPUTE owns this file. No I/O. No `Date.now()`. No `crypto`.
//
// Sign convention (locked):
//   - Asset accounts (`type` ∈ {depository, investment, other}) contribute
//     positively to net worth.
//   - Liability accounts (`type` ∈ {credit, loan}) contribute negatively. A
//     credit-card with `currentBalanceCents = 5000n` reduces net worth by
//     5000n (i.e. it's debt the user owes).
//   - "other" type is treated as an asset. Plaid uses `other` for accounts
//     it cannot classify (HSA, brokerage variants, gift cards, etc.) — at
//     v0.1 we default to the optimistic side and document the choice.
//   - `cashCents` counts ONLY `depository` accounts (positive contribution).
//     Closed accounts and null balances are excluded.
// =============================================================================

import type { Account, NetWorthBreakdownLine, NetWorthResult } from '../types/domain.js';

type AccountType = Account['type'];

const LIABILITY_TYPES: ReadonlySet<AccountType> = new Set(['credit', 'loan']);

/**
 * `true` iff the account should be excluded from compute. Closed accounts are
 * excluded; they remain in the DB for audit history but stop contributing the
 * moment the operator marks them closed.
 */
const isExcluded = (a: Account): boolean => a.closedAt !== null;

const balanceOrZero = (a: Account): bigint => a.currentBalanceCents ?? 0n;

const contributionFor = (type: AccountType): 'asset' | 'liability' => {
  if (LIABILITY_TYPES.has(type)) {
    return 'liability';
  }
  // Default: asset (covers depository, investment, other).
  return 'asset';
};

/**
 * Pure: derive net worth from a list of accounts already filtered by domain
 * scope. Caller is responsible for that scoping (see `RepoScope` in services).
 */
export const netWorth = (input: { readonly accounts: ReadonlyArray<Account> }): NetWorthResult => {
  let assets = 0n;
  let liabilities = 0n;
  let cash = 0n;
  const breakdown: NetWorthBreakdownLine[] = [];

  for (const a of input.accounts) {
    if (isExcluded(a)) {
      continue;
    }
    const balance = balanceOrZero(a);
    const contribution = contributionFor(a.type);

    if (contribution === 'asset') {
      assets += balance;
      if (a.type === 'depository') {
        cash += balance;
      }
    } else {
      liabilities += balance;
    }

    breakdown.push({
      accountId: a.id,
      name: a.name,
      type: a.type,
      balanceCents: balance,
      contribution,
    });
  }

  const netWorthCents = assets - liabilities;

  return {
    assetsCents: assets,
    liabilitiesCents: liabilities,
    netWorthCents,
    cashCents: cash,
    breakdown,
  };
};
