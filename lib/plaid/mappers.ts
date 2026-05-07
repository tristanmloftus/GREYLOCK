// Greylock — Plaid SDK types → domain types
// =============================================================================
// AGENT-PLAID (Phase 3). The single place where Plaid's float-amount
// representation is converted to integer `Cents` (`bigint`). Sign convention
// is preserved as-is from Plaid:
//   - `+amount` = outflow (debit / spending)
//   - `-amount` = inflow  (credit / deposit)
// The compute layer (`lib/compute/month-net.ts`) flips the sign for display.
// =============================================================================

import type {
  AccountBase,
  RemovedTransaction,
  Transaction as PlaidTransaction,
} from 'plaid';

import type {
  AccountId,
  ItemId,
  UserId,
} from '../types/domain.js';
import type {
  Account,
  Cents,
  Domain,
  IsoCurrencyCode,
  PlaidAccountId,
  PlaidTransactionId,
} from '../types/domain.js';
import type { TransactionInput } from '../types/services.js';

// -----------------------------------------------------------------------------
// Float dollars → integer Cents
// -----------------------------------------------------------------------------
//
// Plaid returns monetary values as IEEE-754 floats (e.g. `12.34`). We convert
// to integer cents `bigint` to avoid float drift over large running totals.
//
// Conversion rule:
//   1. Multiply by 100.
//   2. Round half-away-from-zero (so 0.005 → 1 cent for positive amounts and
//      -0.005 → -1 cent for negative; matches POSIX `round()` and is stable
//      under the symmetric flips the compute layer applies).
//   3. Convert to BigInt via `Math.trunc` after rounding (the value is now an
//      integer dollar-cent count by construction).
//
// Edge cases:
//   - `null` input → `null` output (callers handle).
//   - Non-finite (NaN / +/-Infinity) → throw. These are bugs in caller data.
//   - Very large magnitudes: BigInt has no overflow concern at v0.1 scale. We
//     reject |amount| > 1e15 dollars (≈ $1 quadrillion) as a defense-in-depth
//     check against accidental scientific-notation input.
// -----------------------------------------------------------------------------

const MAX_ABS_DOLLARS = 1e15;

/** Branded smart constructor — only used here so the cast is local. */
const PlaidAccountIdCtor = (s: string): PlaidAccountId => s as PlaidAccountId;
const PlaidTransactionIdCtor = (s: string): PlaidTransactionId =>
  s as PlaidTransactionId;

/**
 * Convert a Plaid dollar-amount float to integer cents. Returns null if the
 * input is null. Throws on NaN/Infinity or out-of-range magnitudes.
 */
export function dollarsToCents(dollars: number | null | undefined): Cents | null {
  if (dollars === null || dollars === undefined) {
    return null;
  }
  if (!Number.isFinite(dollars)) {
    throw new Error(`mappers: non-finite Plaid amount`);
  }
  if (Math.abs(dollars) > MAX_ABS_DOLLARS) {
    throw new Error(`mappers: Plaid amount magnitude exceeds limit`);
  }
  // Multiply, then round half-away-from-zero. We avoid `Math.round` (banker's
  // rounding for halves at exactly .5 in some engines is misleading; Node's
  // `Math.round` is half-toward-+Inf which we simulate manually).
  const cents = dollars * 100;
  const rounded = cents >= 0 ? Math.floor(cents + 0.5) : -Math.floor(-cents + 0.5);
  return BigInt(rounded);
}

/** Required-cents variant: throws if the dollars value is null. Use for fields
 *  Plaid contractually returns. */
export function dollarsToCentsRequired(dollars: number | null | undefined): Cents {
  const v = dollarsToCents(dollars);
  if (v === null) {
    throw new Error('mappers: required Plaid amount was null');
  }
  return v;
}

// -----------------------------------------------------------------------------
// Account mapper
// -----------------------------------------------------------------------------

const ACCOUNT_TYPE_MAP: Readonly<Record<string, Account['type']>> = {
  depository: 'depository',
  credit: 'credit',
  loan: 'loan',
  investment: 'investment',
  brokerage: 'investment',
  other: 'other',
};

function mapAccountType(plaidType: string): Account['type'] {
  return ACCOUNT_TYPE_MAP[plaidType] ?? 'other';
}

export interface AccountUpsertInput {
  readonly plaidAccountId: PlaidAccountId;
  readonly name: string;
  readonly officialName: string | null;
  readonly mask: string | null;
  readonly type: Account['type'];
  readonly subtype: string | null;
  readonly isoCurrencyCode: IsoCurrencyCode;
  readonly currentBalanceCents: Cents | null;
  readonly availableBalanceCents: Cents | null;
  readonly limitCents: Cents | null;
}

/**
 * Map a Plaid `AccountBase` to our `AccountRepository.upsertFromPlaid` input
 * shape. v0.1 is USD-only (SPEC §4 decision 7); we coerce non-USD currencies
 * to USD here as a temporary measure (none expected in sandbox/development).
 */
export function mapPlaidAccount(account: AccountBase): AccountUpsertInput {
  const isoCode = account.balances.iso_currency_code;
  if (isoCode !== null && isoCode !== 'USD') {
    // v0.1 is USD-only. The spec does not yet support multi-currency; we
    // surface this as an error rather than silently coercing.
    throw new Error(`mappers: non-USD account currency rejected at v0.1`);
  }
  return {
    plaidAccountId: PlaidAccountIdCtor(account.account_id),
    name: account.name,
    officialName: account.official_name,
    mask: account.mask,
    type: mapAccountType(String(account.type)),
    subtype: account.subtype === null ? null : String(account.subtype),
    isoCurrencyCode: 'USD',
    currentBalanceCents: dollarsToCents(account.balances.current),
    availableBalanceCents: dollarsToCents(account.balances.available),
    limitCents: dollarsToCents(account.balances.limit),
  };
}

// -----------------------------------------------------------------------------
// Transaction mapper
// -----------------------------------------------------------------------------

export interface MapTransactionDeps {
  readonly itemId: ItemId;
  readonly accountIdResolver: (plaidAccountId: PlaidAccountId) => AccountId | null;
  readonly domain: Domain;
  readonly userId: UserId | null;
}

/**
 * Map a Plaid `Transaction` to a `TransactionInput` for
 * `TransactionRepository.applyPlaidSync`. The internal `accountId` (our DB
 * primary key, not Plaid's) is resolved via the supplied callback because the
 * caller has already upserted accounts and knows the mapping.
 *
 * Returns `null` if the resolver cannot find an `accountId` — this should not
 * happen if accounts were upserted before transactions, but we surface it to
 * the caller rather than silently dropping rows.
 */
export function mapPlaidTransaction(
  deps: MapTransactionDeps,
  tx: PlaidTransaction,
): TransactionInput | null {
  const plaidAccountId = PlaidAccountIdCtor(tx.account_id);
  const accountId = deps.accountIdResolver(plaidAccountId);
  if (accountId === null) {
    return null;
  }
  if (tx.iso_currency_code !== null && tx.iso_currency_code !== 'USD') {
    // v0.1 USD-only.
    throw new Error('mappers: non-USD transaction currency rejected at v0.1');
  }
  const date = parsePlaidDate(tx.date);
  const authorizedDate =
    tx.authorized_date === null || tx.authorized_date === undefined
      ? null
      : parsePlaidDate(tx.authorized_date);

  // Plaid sign-convention preserved (positive = outflow, negative = inflow).
  const amountCents = dollarsToCentsRequired(tx.amount);

  // category[] is deprecated but still populated in sandbox; pick the most
  // specific level if present, else null.
  const categoryTop =
    tx.category !== null && tx.category !== undefined && tx.category.length > 0
      ? (tx.category[0] ?? null)
      : null;
  const categoryDetailed =
    tx.category !== null && tx.category !== undefined && tx.category.length > 1
      ? (tx.category[tx.category.length - 1] ?? null)
      : null;

  return {
    itemId: deps.itemId,
    accountId,
    domain: deps.domain,
    userId: deps.userId,
    plaidTransactionId: PlaidTransactionIdCtor(tx.transaction_id),
    amountCents,
    isoCurrencyCode: 'USD',
    date,
    authorizedDate,
    name: tx.name,
    merchantName: tx.merchant_name ?? null,
    pending: tx.pending,
    category: categoryTop,
    categoryDetailed,
  };
}

/**
 * Map a Plaid `RemovedTransaction` to our `PlaidTransactionId` brand. The
 * caller passes the resulting list to
 * `TransactionRepository.applyPlaidSync` as `removedPlaidIds`.
 */
export function mapRemovedTransactionId(rt: RemovedTransaction): PlaidTransactionId {
  return PlaidTransactionIdCtor(rt.transaction_id);
}

// -----------------------------------------------------------------------------
// Date parsing
// -----------------------------------------------------------------------------
//
// Plaid returns dates in `YYYY-MM-DD` (UTC date-only). We parse to a `Date`
// at midnight UTC; the compute layer treats these as exclusive end-of-day for
// rolling-window math. Returning `Date(NaN)` from a malformed input would
// silently corrupt downstream comparisons, so we throw.
// -----------------------------------------------------------------------------

function parsePlaidDate(s: string): Date {
  const matched = /^(\d{4})-(\d{2})-(\d{2})$/u.exec(s);
  if (matched === null) {
    throw new Error('mappers: malformed Plaid date');
  }
  const yStr = matched[1];
  const mStr = matched[2];
  const dStr = matched[3];
  if (yStr === undefined || mStr === undefined || dStr === undefined) {
    throw new Error('mappers: malformed Plaid date');
  }
  const y = Number.parseInt(yStr, 10);
  const m = Number.parseInt(mStr, 10);
  const d = Number.parseInt(dStr, 10);
  if (!Number.isFinite(y) || !Number.isFinite(m) || !Number.isFinite(d)) {
    throw new Error('mappers: malformed Plaid date');
  }
  return new Date(Date.UTC(y, m - 1, d));
}
