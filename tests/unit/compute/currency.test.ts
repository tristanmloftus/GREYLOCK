// Tests for `lib/compute/currency.ts`.

import { describe, it, expect } from 'vitest';

import { centsAbs, centsToDisplay, toCents } from '../../../lib/compute/currency.js';
import type { Cents } from '../../../lib/types/domain.js';

describe('toCents — accepted shapes', () => {
  it('parses whole-dollar strings', () => {
    expect(toCents('0')).toBe(0n);
    expect(toCents('1')).toBe(100n);
    expect(toCents('1234')).toBe(123_400n);
  });

  it('parses standard 2-decimal dollar strings', () => {
    expect(toCents('1234.56')).toBe(123_456n);
    expect(toCents('0.00')).toBe(0n);
    expect(toCents('0.01')).toBe(1n);
    expect(toCents('0.99')).toBe(99n);
  });

  it('zero-pads single-decimal values', () => {
    expect(toCents('1.5')).toBe(150n);
    expect(toCents('100.0')).toBe(10_000n);
  });

  it('treats trailing dot as .00', () => {
    expect(toCents('5.')).toBe(500n);
  });

  it('treats leading dot as 0.<frac>', () => {
    expect(toCents('.50')).toBe(50n);
    expect(toCents('.5')).toBe(50n);
  });

  it('parses negative values', () => {
    expect(toCents('-50.00')).toBe(-5_000n);
    expect(toCents('-0.01')).toBe(-1n);
    expect(toCents('-1234.56')).toBe(-123_456n);
  });

  it('parses explicitly positive values', () => {
    expect(toCents('+10.00')).toBe(1_000n);
    expect(toCents('+0.05')).toBe(5n);
  });

  it('tolerates surrounding whitespace', () => {
    expect(toCents('  100.00  ')).toBe(10_000n);
    expect(toCents('\t-5.00\n')).toBe(-500n);
  });
});

describe('toCents — rejected inputs', () => {
  it('rejects empty / whitespace-only', () => {
    expect(() => toCents('')).toThrow();
    expect(() => toCents('   ')).toThrow();
  });

  it('rejects more than 2 fractional digits', () => {
    expect(() => toCents('1.234')).toThrow(/2 fractional digits/);
  });

  it('rejects multiple decimal points', () => {
    expect(() => toCents('1.2.3')).toThrow(/multiple decimal points/);
  });

  it('rejects letters and currency symbols', () => {
    expect(() => toCents('$1.00')).toThrow();
    expect(() => toCents('1.00 USD')).toThrow();
    expect(() => toCents('NaN')).toThrow();
    expect(() => toCents('Infinity')).toThrow();
  });

  it('rejects scientific notation', () => {
    expect(() => toCents('1e3')).toThrow();
  });

  it('rejects thousands separators', () => {
    expect(() => toCents('1,234.56')).toThrow();
  });

  it('rejects sign with no body', () => {
    expect(() => toCents('-')).toThrow();
    expect(() => toCents('+')).toThrow();
  });

  it('rejects non-digit characters in fractional segment', () => {
    expect(() => toCents('1.5a')).toThrow();
    expect(() => toCents('1.ab')).toThrow();
  });

  it('rejects a non-string', () => {
    // @ts-expect-error: intentional bad type
    expect(() => toCents(undefined)).toThrow();
    // @ts-expect-error: intentional bad type
    expect(() => toCents(null)).toThrow();
    // @ts-expect-error: intentional bad type
    expect(() => toCents(123)).toThrow();
  });
});

describe('centsToDisplay — basic formatting', () => {
  it('formats positive values', () => {
    expect(centsToDisplay(12_345n as Cents)).toBe('$123.45');
    expect(centsToDisplay(0n as Cents)).toBe('$0.00');
    expect(centsToDisplay(1n as Cents)).toBe('$0.01');
    expect(centsToDisplay(99n as Cents)).toBe('$0.99');
    expect(centsToDisplay(100n as Cents)).toBe('$1.00');
  });

  it('formats negative values with sign before symbol', () => {
    expect(centsToDisplay(-50n as Cents)).toBe('-$0.50');
    expect(centsToDisplay(-12_345n as Cents)).toBe('-$123.45');
    expect(centsToDisplay(-1n as Cents)).toBe('-$0.01');
  });

  it('handles very large bigint values without precision loss', () => {
    // $10 billion in cents
    const tenB: Cents = 1_000_000_000_000n as Cents;
    expect(centsToDisplay(tenB)).toBe('$10000000000.00');
  });
});

describe('centsToDisplay — sign option', () => {
  it("emits '+' for positive when sign=always", () => {
    expect(centsToDisplay(12_345n as Cents, { sign: 'always' })).toBe('+$123.45');
    expect(centsToDisplay(1n as Cents, { sign: 'always' })).toBe('+$0.01');
  });

  it("does not emit '+' for zero even when sign=always", () => {
    expect(centsToDisplay(0n as Cents, { sign: 'always' })).toBe('$0.00');
  });

  it('keeps negative sign regardless of option', () => {
    expect(centsToDisplay(-50n as Cents, { sign: 'always' })).toBe('-$0.50');
    expect(centsToDisplay(-50n as Cents, { sign: 'never' })).toBe('-$0.50');
  });
});

describe('centsToDisplay — currency option', () => {
  it("omits '$' when currency=false", () => {
    expect(centsToDisplay(12_345n as Cents, { currency: false })).toBe('123.45');
    expect(centsToDisplay(-50n as Cents, { currency: false })).toBe('-0.50');
    expect(centsToDisplay(0n as Cents, { currency: false })).toBe('0.00');
  });

  it("emits '$' by default and when currency=true", () => {
    expect(centsToDisplay(12_345n as Cents, { currency: true })).toBe('$123.45');
  });

  it('combines sign=always and currency=false', () => {
    expect(centsToDisplay(12_345n as Cents, { sign: 'always', currency: false })).toBe('+123.45');
    expect(centsToDisplay(-12_345n as Cents, { sign: 'always', currency: false })).toBe('-123.45');
  });
});

describe('toCents <-> centsToDisplay round-trip', () => {
  const cases = ['0.00', '0.01', '1.00', '1234.56', '10000000.00', '0.99'];
  for (const s of cases) {
    it(`round-trips "${s}"`, () => {
      const c = toCents(s);
      expect(centsToDisplay(c, { currency: false })).toBe(s);
    });
  }

  const negCases = ['-0.01', '-1.00', '-1234.56'];
  for (const s of negCases) {
    it(`round-trips "${s}" (negative)`, () => {
      const c = toCents(s);
      expect(centsToDisplay(c, { currency: false })).toBe(s);
    });
  }
});

describe('centsAbs', () => {
  it('returns the same value for zero', () => {
    expect(centsAbs(0n as Cents)).toBe(0n);
  });

  it('returns the same value for positives', () => {
    expect(centsAbs(123n as Cents)).toBe(123n);
    expect(centsAbs(999_999_999n as Cents)).toBe(999_999_999n);
  });

  it('flips negatives', () => {
    expect(centsAbs(-1n as Cents)).toBe(1n);
    expect(centsAbs(-999_999_999n as Cents)).toBe(999_999_999n);
  });
});
