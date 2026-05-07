// Greylock — allowlist + placeholder rejection tests
// =============================================================================
// AGENT-AUTH (Phase 2). Covers the case-insensitive trim contract and the
// hard-rejection of `cade-placeholder@greylock.invalid`.
// =============================================================================

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import {
  PLACEHOLDER_EMAIL,
  isAllowedEmail,
  isPlaceholderEmail,
  normalizeEmail,
  readAllowedEmails,
} from '../../../lib/auth/allowlist.js';

const ORIG = process.env['ALLOWED_EMAILS'];

describe('allowlist', () => {
  beforeEach(() => {
    process.env['ALLOWED_EMAILS'] =
      'rory.patrick.loftus@gmail.com,tristan.m.loftus@gmail.com,cade-placeholder@greylock.invalid';
  });
  afterEach(() => {
    if (ORIG === undefined) {
      delete process.env['ALLOWED_EMAILS'];
    } else {
      process.env['ALLOWED_EMAILS'] = ORIG;
    }
  });

  it('normalizes emails (trim + lowercase)', () => {
    expect(normalizeEmail('  Rory@EXAMPLE.com ')).toBe('rory@example.com');
  });

  it('reads the allowlist into a Set, dropping empty entries', () => {
    process.env['ALLOWED_EMAILS'] = 'a@b.com,,B@C.com,';
    const set = readAllowedEmails();
    expect(set.has('a@b.com')).toBe(true);
    expect(set.has('b@c.com')).toBe(true);
    expect(set.size).toBe(2);
  });

  it('matches case-insensitively after trim', () => {
    expect(isAllowedEmail(' RORY.patrick.loftus@gmail.com ')).toBe(true);
  });

  it('rejects an unallowed email', () => {
    expect(isAllowedEmail('intruder@example.com')).toBe(false);
  });

  it('hard-rejects the placeholder address even if it is in the env', () => {
    expect(isPlaceholderEmail(PLACEHOLDER_EMAIL)).toBe(true);
    expect(isAllowedEmail(PLACEHOLDER_EMAIL)).toBe(false);
    expect(isAllowedEmail(' Cade-Placeholder@GREYLOCK.invalid')).toBe(false);
  });

  it('returns false on empty allowlist env', () => {
    process.env['ALLOWED_EMAILS'] = '';
    expect(isAllowedEmail('rory.patrick.loftus@gmail.com')).toBe(false);
  });

  it('returns false on missing allowlist env', () => {
    delete process.env['ALLOWED_EMAILS'];
    expect(isAllowedEmail('rory.patrick.loftus@gmail.com')).toBe(false);
  });
});
