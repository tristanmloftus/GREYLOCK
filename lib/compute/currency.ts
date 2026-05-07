// Greylock — USD currency helpers (Cents <-> display string)
// =============================================================================
// AGENT-COMPUTE owns this file. Pure, no I/O.
//
// SPEC §4 Decision 7: USD only for v0.1. Multi-currency is deferred.
// All money is `bigint` Cents (minor units). Float arithmetic is forbidden;
// this module's only float exposure is parsing the user-supplied dollar
// string in `toCents`, which is done lexically (digit-by-digit) and then
// converted to bigint without ever flowing through Number for the math.
// =============================================================================

import type { Cents } from '../types/domain.js';

const CENTS_PER_DOLLAR = 100n;

/**
 * Parse a USD dollar string to Cents.
 *
 * Accepted shapes (case-insensitive whitespace tolerated only at the edges):
 *   "0", "0.0", "0.00"
 *   "1234", "1234.5", "1234.56"
 *   "-50.00", "-0.05"
 *   "+10.00"
 *
 * Rejected:
 *   - empty / whitespace-only
 *   - non-finite numerics ("NaN", "Infinity", "1e3")
 *   - more than 2 fractional digits ("1.234")
 *   - non-digit characters anywhere except a single leading sign and one '.'
 *   - currency symbols ("$1.00")
 *   - thousands separators ("1,234.56")
 *
 * Throws on bad input — callers MUST handle the throw or pre-validate.
 */
export const toCents = (dollarString: string): Cents => {
  if (typeof dollarString !== 'string') {
    throw new TypeError('toCents: input must be a string');
  }
  const trimmed = dollarString.trim();
  if (trimmed.length === 0) {
    throw new Error('toCents: empty string');
  }

  // Optional leading sign.
  let sign: 1n | -1n = 1n;
  let body = trimmed;
  if (body.startsWith('+')) {
    body = body.slice(1);
  } else if (body.startsWith('-')) {
    sign = -1n;
    body = body.slice(1);
  }

  if (body.length === 0) {
    throw new Error(`toCents: invalid input "${dollarString}"`);
  }

  // Split on the decimal point. Reject more than one '.'.
  const dotIndex = body.indexOf('.');
  let dollarsPart: string;
  let centsPart: string;
  if (dotIndex < 0) {
    dollarsPart = body;
    centsPart = '00';
  } else {
    if (body.indexOf('.', dotIndex + 1) >= 0) {
      throw new Error(`toCents: multiple decimal points in "${dollarString}"`);
    }
    dollarsPart = body.slice(0, dotIndex);
    centsPart = body.slice(dotIndex + 1);
    if (centsPart.length === 0) {
      // "1." — treat as "1.00" rather than rejecting outright.
      centsPart = '00';
    } else if (centsPart.length > 2) {
      throw new Error(`toCents: more than 2 fractional digits in "${dollarString}"`);
    } else if (centsPart.length === 1) {
      centsPart = `${centsPart}0`;
    }
  }

  // Dollar part may be empty if input was just ".50" — accept that.
  if (dollarsPart.length === 0) {
    dollarsPart = '0';
  }

  if (!/^[0-9]+$/.test(dollarsPart)) {
    throw new Error(`toCents: invalid dollars segment in "${dollarString}"`);
  }
  if (!/^[0-9]{2}$/.test(centsPart)) {
    throw new Error(`toCents: invalid cents segment in "${dollarString}"`);
  }

  const dollars = BigInt(dollarsPart);
  const cents = BigInt(centsPart);
  const total = dollars * CENTS_PER_DOLLAR + cents;
  return (sign * total) as Cents;
};

/**
 * Absolute value of a Cents value. Pure, branchless.
 */
export const centsAbs = (c: Cents): Cents => ((c < 0n ? -c : c) as Cents);

export interface CentsToDisplayOptions {
  /** Whether to always emit a sign for non-negative values. */
  readonly sign?: 'always' | 'never';
  /**
   * Whether to prefix with the USD currency symbol.
   * Default: true. Set to `false` to omit the leading "$".
   */
  readonly currency?: boolean;
}

/**
 * Format a Cents value as a USD display string.
 *
 *   12345n  -> "$123.45"
 *   -50n    -> "-$0.50"
 *   0n      -> "$0.00"
 *
 * `sign: 'always'` flips positives to use a leading "+":
 *   12345n  -> "+$123.45"
 *   0n      -> "$0.00"  (zero is neither positive nor negative)
 *
 * `currency: false` omits the "$":
 *   12345n  -> "123.45"
 *   -50n    -> "-0.50"
 */
export const centsToDisplay = (c: Cents, opts?: CentsToDisplayOptions): string => {
  const includeCurrency = opts?.currency !== false;
  const showAlwaysSign = opts?.sign === 'always';

  const negative = c < 0n;
  const absVal = negative ? -c : c;
  const absDollars = absVal / CENTS_PER_DOLLAR;
  const absCents = absVal % CENTS_PER_DOLLAR;
  const absCentsStr = absCents.toString().padStart(2, '0');

  const numeric = `${absDollars.toString()}.${absCentsStr}`;
  const symbol = includeCurrency ? '$' : '';

  if (negative) {
    return `-${symbol}${numeric}`;
  }
  if (showAlwaysSign && c > 0n) {
    return `+${symbol}${numeric}`;
  }
  return `${symbol}${numeric}`;
};
