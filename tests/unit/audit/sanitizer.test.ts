// Greylock — sanitizer unit tests
// =============================================================================
// AGENT-AUDIT-LOG. Exhaustive deny/allow coverage. Sanitizer never throws.
// =============================================================================

import { describe, expect, it } from 'vitest';

import { sanitizeDetails } from '../../../lib/audit/sanitizer.js';

describe('sanitizeDetails — deny on key match', () => {
  const denied: ReadonlyArray<readonly [string, Record<string, unknown>]> = [
    ['password', { password: 'x' }],
    ['passphrase', { passphrase: 'x' }],
    ['secret', { secret: 'x' }],
    ['cookie', { cookie: 'session=abc' }],
    ['dek', { dek: 'x' }],
    ['kek', { kek: 'x' }],
    ['kekSalt', { kekSalt: 'x' }],
    ['credentialPublicKey', { credentialPublicKey: 'x' }],
    ['signature', { signature: 'x' }],
    ['attestationObject', { attestationObject: 'x' }],
    ['clientDataJSON', { clientDataJSON: 'x' }],
    ['authenticatorData', { authenticatorData: 'x' }],
    ['pem', { pem: 'x' }],
    // 'token' substring rejection — covers access_token, refresh_token, link_token.
    ['access_token', { access_token: 'x' }],
    ['refresh_token', { refresh_token: 'x' }],
    ['link_token', { link_token: 'x' }],
    ['enrollmentToken', { enrollmentToken: 'x' }],
    // Bare 'key' rejected (not allowlisted).
    ['masterKey', { masterKey: 'x' }],
    // Case-insensitive match.
    ['DEK', { DEK: 'x' }],
    ['Password', { Password: 'x' }],
  ];

  for (const [name, payload] of denied) {
    it(`rejects key '${name}'`, () => {
      const result = sanitizeDetails(payload);
      expect(result.ok).toBe(false);
      if (!result.ok) {
        expect(result.reason).toMatch(/deny key/);
      }
    });
  }
});

describe('sanitizeDetails — allowlist override', () => {
  it('accepts opaque identifiers + control fields', () => {
    const result = sanitizeDetails({
      userId: 'u_abc',
      sessionId: 's_xyz',
      itemId: 'i_q',
      accountId: 'a_q',
      transactionId: 't_q',
      passkeyId: 'p_q',
      snapshotId: 'sn_q',
      tokenId: 'tk_row_id', // row id, NOT the plaintext token
      subjectId: 'subj',
      actorUserId: 'u_abc',
      domain: 'personal',
      outcome: 'success',
      action: 'session_created',
      kind: 'fixed_window',
      reason: 'login',
      version: 3,
      seq: '1234',
      ts: '2026-01-01T00:00:00Z',
      count: 5,
      added: 12,
      modified: 0,
      removed: 0,
      httpStatus: 200,
      errorCode: 'NONE',
    });
    expect(result.ok).toBe(true);
  });
});

describe('sanitizeDetails — value-based token-shape rejection', () => {
  it('rejects 32+ char base64url string at any key', () => {
    const result = sanitizeDetails({ reason: 'a'.repeat(40) });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/token-shape/);
    }
  });

  it('rejects 32+ char hex string', () => {
    const result = sanitizeDetails({ reason: 'a'.repeat(64) });
    expect(result.ok).toBe(false);
  });

  it('rejects 64-char hex token', () => {
    // 64 hex chars = a SHA-256 hash. Classic token-shape value.
    const sha256Hex = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';
    const result = sanitizeDetails({ reason: sha256Hex });
    expect(result.ok).toBe(false);
  });

  it('rejects 64-char base64url token', () => {
    const b64 = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA';
    const result = sanitizeDetails({ reason: b64 });
    expect(result.ok).toBe(false);
  });

  it('accepts a short opaque id under 32 chars', () => {
    const result = sanitizeDetails({ reason: 'x'.repeat(31) });
    expect(result.ok).toBe(true);
  });

  it('rejects token-shape value even at allowlisted key', () => {
    // The allowlist is for KEYS. Token-shape VALUES still fail wherever they
    // appear — that's the defense-in-depth contract.
    const result = sanitizeDetails({ userId: 'a'.repeat(40) });
    expect(result.ok).toBe(false);
  });
});

describe('sanitizeDetails — depth + size limits', () => {
  it('rejects nested object at depth 9', () => {
    let inner: Record<string, unknown> = { reason: 'leaf' };
    // Wrap 9 levels (root counts as 0 → key 'reason' would be at depth 10 ≥ MAX_DEPTH).
    for (let i = 0; i < 9; i++) {
      inner = { reason: inner };
    }
    const result = sanitizeDetails(inner);
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/depth/);
    }
  });

  it('accepts moderate nesting (depth 5)', () => {
    let inner: Record<string, unknown> = { reason: 'leaf' };
    for (let i = 0; i < 5; i++) {
      inner = { reason: inner };
    }
    const result = sanitizeDetails(inner);
    expect(result.ok).toBe(true);
  });

  it('rejects payloads larger than 64 KiB', () => {
    // Build a string-allowed payload over the cap. Use short repeating
    // 31-char strings to dodge the token-shape detector.
    const chunk = 'x'.repeat(31);
    const arr: string[] = [];
    while (arr.length < 3000) {
      arr.push(chunk);
    }
    const result = sanitizeDetails({ reason: arr });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/64 KiB/);
    }
  });

  it('accepts payloads under the 64 KiB cap', () => {
    const result = sanitizeDetails({ reason: 'x'.repeat(31), count: 1 });
    expect(result.ok).toBe(true);
  });
});

describe('sanitizeDetails — BigInt handling', () => {
  it('stringifies BigInt at allowlisted key', () => {
    const result = sanitizeDetails({ count: 9007199254740993n });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.sanitized['count']).toBe('9007199254740993');
    }
  });

  it('stringifies BigInt at allowlisted key (storedCounter)', () => {
    const result = sanitizeDetails({ storedCounter: 5n, newCounter: 6n });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.sanitized['storedCounter']).toBe('5');
      expect(result.sanitized['newCounter']).toBe('6');
    }
  });

  it('rejects BigInt at a key that passes the key check but is not allowlisted', () => {
    // 'rawData' is not in ALLOWED_KEYS and doesn't match any deny substring.
    // The key passes; the BigInt walk then rejects because there's no
    // allowlist entry to authorize stringification.
    const result = sanitizeDetails({ rawData: 42n });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/bigint/);
    }
  });
});

describe('sanitizeDetails — unsafe value kinds', () => {
  it('rejects Buffer values', () => {
    const result = sanitizeDetails({ reason: Buffer.from([1, 2, 3]) });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/binary/);
    }
  });

  it('rejects Uint8Array values', () => {
    const result = sanitizeDetails({ reason: new Uint8Array([1, 2, 3]) });
    expect(result.ok).toBe(false);
  });

  it('rejects Function values', () => {
    const result = sanitizeDetails({ reason: () => 1 });
    expect(result.ok).toBe(false);
  });

  it('rejects Symbol values', () => {
    const result = sanitizeDetails({ reason: Symbol('x') });
    expect(result.ok).toBe(false);
  });

  it('rejects undefined values explicitly', () => {
    const result = sanitizeDetails({ reason: undefined });
    expect(result.ok).toBe(false);
  });

  it('rejects non-finite numbers', () => {
    expect(sanitizeDetails({ count: Number.POSITIVE_INFINITY }).ok).toBe(false);
    expect(sanitizeDetails({ count: Number.NaN }).ok).toBe(false);
  });
});

describe('sanitizeDetails — nested + arrays', () => {
  it('walks nested objects', () => {
    const result = sanitizeDetails({
      reason: 'login',
      transports: ['usb', 'internal'],
      action: 'session_created',
    });
    expect(result.ok).toBe(true);
  });

  it('rejects deny key inside a nested object (whole rejection, no partial-strip)', () => {
    const result = sanitizeDetails({
      reason: 'inner',
      added: 1,
      // Nested deny.
      kind: { password: 'leaked' },
    });
    expect(result.ok).toBe(false);
  });

  it('rejects token-shape value inside an array', () => {
    const result = sanitizeDetails({
      transports: ['usb', 'a'.repeat(40)],
    });
    expect(result.ok).toBe(false);
  });

  it('preserves clean nested + array payload', () => {
    const result = sanitizeDetails({
      action: 'plaid_sync_completed',
      added: 12n,
      modified: 0,
      removed: 0,
      transports: ['usb', 'internal'],
    });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.sanitized['added']).toBe('12');
      expect(result.sanitized['transports']).toEqual(['usb', 'internal']);
    }
  });
});

describe('sanitizeDetails — never throws', () => {
  it('does not throw on a getter that throws', () => {
    const obj: Record<string, unknown> = {};
    Object.defineProperty(obj, 'reason', {
      get() {
        throw new Error('boom');
      },
      enumerable: true,
    });
    // Must surface as a Result, not a thrown exception.
    expect(() => sanitizeDetails(obj)).not.toThrow();
    const result = sanitizeDetails(obj);
    expect(result.ok).toBe(false);
  });

  it('does not throw on a Date object (rejected as non-plain)', () => {
    const result = sanitizeDetails({ reason: new Date() });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.reason).toMatch(/non-plain/);
    }
  });

  it('does not throw on null', () => {
    const result = sanitizeDetails({ reason: null });
    expect(result.ok).toBe(true);
  });

  it('accepts empty object', () => {
    const result = sanitizeDetails({});
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.sanitized).toEqual({});
    }
  });
});
