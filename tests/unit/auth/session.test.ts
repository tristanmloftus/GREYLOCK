// Greylock — session lifecycle tests (mocked time + repos)
// =============================================================================
// AGENT-AUTH (Phase 2). Covers idle expiry, absolute expiry, sliding window,
// single-active-session lookup, and revoke-with-DEK-unload semantics. Crypto
// and SessionRepository are mocked.
// =============================================================================

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { Err, Ok } from '../../../lib/types/domain.js';
import { SessionId, UserId } from '../../../lib/types/domain.js';
import type {
  Session,
  UserId as UserIdType,
  SessionId as SessionIdType,
} from '../../../lib/types/domain.js';
import type { CryptoService, SessionRepository } from '../../../lib/types/services.js';
import {
  buildCookieAttributes,
  createSession,
  enforceIdleAndAbsoluteTimeouts,
  readSessionConfig,
  revokeSession,
  sealSessionCookie,
  unsealSessionCookie,
  validateSession,
  type SessionConfig,
  type SessionDeps,
} from '../../../lib/auth/session.js';

const SECRET =
  'this-is-a-32-byte-or-longer-test-only-secret-string-not-used-in-production';

const baseConfig: SessionConfig = {
  cookieName: 'greylock_session',
  secret: SECRET,
  idleMinutes: 30,
  absoluteHours: 8,
};

beforeEach(() => {
  process.env['SESSION_COOKIE_NAME'] = 'greylock_session';
  process.env['SESSION_SECRET'] = SECRET;
  process.env['SESSION_IDLE_MINUTES'] = '30';
  process.env['SESSION_ABSOLUTE_HOURS'] = '8';
});

afterEach(() => {
  delete process.env['SESSION_COOKIE_NAME'];
  delete process.env['SESSION_SECRET'];
  delete process.env['SESSION_IDLE_MINUTES'];
  delete process.env['SESSION_ABSOLUTE_HOURS'];
});

// -----------------------------------------------------------------------------
// Mock factories
// -----------------------------------------------------------------------------

interface MockSessionStore {
  sessions: Map<string, Session>;
  revoked: string[];
  touched: Array<{ id: string; newIdleTimeoutAt: Date }>;
}

function newStore(): MockSessionStore {
  return { sessions: new Map(), revoked: [], touched: [] };
}

function mockSessionRepo(store: MockSessionStore): SessionRepository {
  return {
    create: async ({ userId, expiresAt, idleTimeoutAt, userAgent, remoteAddr }) => {
      const id = SessionId(`sess_${String(store.sessions.size + 1)}`);
      const sess: Session = {
        id,
        userId,
        status: 'active',
        createdAt: new Date(),
        lastActivityAt: new Date(),
        expiresAt,
        idleTimeoutAt,
        revokedAt: null,
        revokedReason: null,
        userAgent,
        remoteAddr,
      };
      store.sessions.set(id, sess);
      return Ok(sess);
    },
    findActiveById: async (id) => {
      const found = store.sessions.get(id);
      if (found === undefined) {
        return Ok(null);
      }
      if (found.status !== 'active') {
        return Ok(null);
      }
      return Ok(found);
    },
    findActiveByUser: async (userId) => {
      for (const sess of store.sessions.values()) {
        if (sess.userId === userId && sess.status === 'active') {
          return Ok(sess);
        }
      }
      return Ok(null);
    },
    touch: async ({ id, newIdleTimeoutAt }) => {
      store.touched.push({ id, newIdleTimeoutAt });
      const found = store.sessions.get(id);
      if (found === undefined) {
        return Err({ kind: 'not_found' });
      }
      store.sessions.set(id, { ...found, idleTimeoutAt: newIdleTimeoutAt });
      return Ok(undefined);
    },
    revoke: async ({ id, reason }) => {
      store.revoked.push(id);
      const found = store.sessions.get(id);
      if (found === undefined) {
        return Err({ kind: 'not_found' });
      }
      store.sessions.set(id, {
        ...found,
        status: 'revoked',
        revokedAt: new Date(),
        revokedReason: reason,
      });
      return Ok(undefined);
    },
    revokeAllActive: async ({ reason }) => {
      let count = 0;
      for (const [id, sess] of store.sessions.entries()) {
        if (sess.status === 'active') {
          store.sessions.set(id, {
            ...sess,
            status: 'revoked',
            revokedAt: new Date(),
            revokedReason: reason,
          });
          count += 1;
        }
      }
      return Ok({ count });
    },
    expireOverdue: async () => Ok({ count: 0 }),
  };
}

function mockCrypto(): CryptoService & { unloaded: UserIdType[] } {
  const unloaded: UserIdType[] = [];
  return {
    initializeFromKeychain: async () => Ok(undefined),
    shutdown: async () => undefined,
    loadUserDek: async () => Ok(undefined),
    unloadUserDek: async (userId: UserIdType) => {
      unloaded.push(userId);
    },
    hasUserDek: () => false,
    hasPccDek: () => true,
    encrypt: async () => Err({ kind: 'aad_mismatch' }),
    decrypt: async () => Err({ kind: 'aad_mismatch' }),
    wrapUserDek: async () => Err({ kind: 'kdf_failure' }),
    rotateUserDek: async () => Err({ kind: 'kdf_failure' }),
    rotateMaster: async () => Err({ kind: 'rotation_in_progress' }),
    unloaded,
  } as unknown as CryptoService & { unloaded: UserIdType[] };
}

function depsWithClock(now: Date, store: MockSessionStore, crypto: CryptoService): SessionDeps {
  return {
    sessionRepo: mockSessionRepo(store),
    crypto,
    config: baseConfig,
    now: () => now,
    randomNonce: () => 'fixed-nonce',
  };
}

// -----------------------------------------------------------------------------
// readSessionConfig
// -----------------------------------------------------------------------------

describe('readSessionConfig', () => {
  it('reads from process.env', () => {
    const c = readSessionConfig();
    expect(c.cookieName).toBe('greylock_session');
    expect(c.idleMinutes).toBe(30);
    expect(c.absoluteHours).toBe(8);
  });
  it('throws on missing secret', () => {
    delete process.env['SESSION_SECRET'];
    expect(() => readSessionConfig()).toThrow();
  });
  it('throws on too-short secret', () => {
    process.env['SESSION_SECRET'] = 'short';
    expect(() => readSessionConfig()).toThrow();
  });
});

// -----------------------------------------------------------------------------
// Cookie attributes
// -----------------------------------------------------------------------------

describe('cookie attributes', () => {
  it('locks SameSite=Strict, Secure, HttpOnly, Path=/', () => {
    const a = buildCookieAttributes(baseConfig);
    expect(a.sameSite).toBe('strict');
    expect(a.secure).toBe(true);
    expect(a.httpOnly).toBe(true);
    expect(a.path).toBe('/');
    expect(a.maxAge).toBe(8 * 60 * 60);
  });
});

// -----------------------------------------------------------------------------
// Seal / unseal
// -----------------------------------------------------------------------------

describe('cookie seal/unseal', () => {
  it('round-trips a session body', async () => {
    const sealed = await sealSessionCookie({
      body: { sessionId: SessionId('sess_x'), nonce: 'n' },
      config: baseConfig,
    });
    const out = await unsealSessionCookie({ cookieValue: sealed, config: baseConfig });
    expect(out.ok).toBe(true);
    if (out.ok) {
      expect(out.value.sessionId).toBe('sess_x');
      expect(out.value.nonce).toBe('n');
    }
  });

  it('rejects garbage', async () => {
    const out = await unsealSessionCookie({ cookieValue: 'not-a-cookie', config: baseConfig });
    expect(out.ok).toBe(false);
  });
});

// -----------------------------------------------------------------------------
// createSession + sliding window + idle / absolute timeouts
// -----------------------------------------------------------------------------

describe('createSession + validateSession', () => {
  it('creates a session with idle=+30m, absolute=+8h, and a sealed cookie', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');

    const created = await createSession(deps, { userId, userAgent: 'ua', remoteAddr: null });
    expect(created.ok).toBe(true);
    if (!created.ok) {return;}
    expect(created.value.session.idleTimeoutAt.getTime() - t0.getTime()).toBe(30 * 60 * 1000);
    expect(created.value.session.expiresAt.getTime() - t0.getTime()).toBe(8 * 60 * 60 * 1000);

    // Validate at t0+10m: sliding should bump idleTimeoutAt to t+40m.
    const t1 = new Date(t0.getTime() + 10 * 60 * 1000);
    const deps1 = depsWithClock(t1, store, crypto);
    const validated = await validateSession(deps1, {
      sessionId: created.value.session.id,
      cookieValue: created.value.cookieValue,
    });
    expect(validated.ok).toBe(true);
    if (validated.ok) {
      expect(validated.value.idleTimeoutAt.getTime() - t1.getTime()).toBe(30 * 60 * 1000);
    }
  });

  it('rejects on idle expiry', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');
    const created = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    expect(created.ok).toBe(true);
    if (!created.ok) {return;}

    // Move past idle window.
    const t1 = new Date(t0.getTime() + 31 * 60 * 1000);
    const deps1 = depsWithClock(t1, store, crypto);
    const result = await validateSession(deps1, {
      sessionId: created.value.session.id,
      cookieValue: created.value.cookieValue,
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('session_expired');
    }
    // The session should have been revoked AND DEK unloaded (no other active).
    expect((crypto as unknown as { unloaded: UserIdType[] }).unloaded).toContain(userId);
  });

  it('rejects on absolute expiry even if idle was just touched', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');
    const created = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    if (!created.ok) {throw new Error('setup');}

    // Hand-craft a session row that has been kept alive but is past absolute.
    const sess = created.value.session;
    store.sessions.set(sess.id, {
      ...sess,
      idleTimeoutAt: new Date(t0.getTime() + 9 * 60 * 60 * 1000), // idle in future
    });

    const t1 = new Date(t0.getTime() + 8 * 60 * 60 * 1000 + 1);
    const deps1 = depsWithClock(t1, store, crypto);
    const result = await validateSession(deps1, {
      sessionId: sess.id,
      cookieValue: created.value.cookieValue,
    });
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.kind).toBe('session_expired');
    }
  });

  it('does not unload DEK if another active session exists for the same user', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');

    // Create two sessions for the same user.
    const a = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    const b = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    if (!a.ok || !b.ok) {throw new Error('setup');}

    // Expire `a`'s idle timeout.
    const t1 = new Date(t0.getTime() + 31 * 60 * 1000);
    const deps1 = depsWithClock(t1, store, crypto);
    // First, restore `b` to active in store with future idle (the constructor
    // gave both the same timeouts; re-establish `b` with t1+30m).
    store.sessions.set(b.value.session.id, {
      ...b.value.session,
      idleTimeoutAt: new Date(t1.getTime() + 30 * 60 * 1000),
      expiresAt: new Date(t1.getTime() + 8 * 60 * 60 * 1000),
    });

    const result = await validateSession(deps1, {
      sessionId: a.value.session.id,
      cookieValue: a.value.cookieValue,
    });
    expect(result.ok).toBe(false);
    // DEK unload should NOT have been called because b is still active.
    expect((crypto as unknown as { unloaded: UserIdType[] }).unloaded).not.toContain(userId);
  });

  it('rejects when the cookie sessionId does not match the supplied sessionId', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const created = await createSession(deps, {
      userId: UserId('u1'),
      userAgent: null,
      remoteAddr: null,
    });
    if (!created.ok) {throw new Error('setup');}

    const result = await validateSession(deps, {
      sessionId: SessionId('different'),
      cookieValue: created.value.cookieValue,
    });
    expect(result.ok).toBe(false);
  });
});

// -----------------------------------------------------------------------------
// revokeSession
// -----------------------------------------------------------------------------

describe('revokeSession', () => {
  it('revokes and unloads DEK if no other active session', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');
    const created = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    if (!created.ok) {throw new Error('setup');}

    const result = await revokeSession(deps, {
      sessionId: created.value.session.id,
      reason: 'logout',
    });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.dekUnloaded).toBe(true);
      expect(result.value.userId).toBe(userId);
    }
  });

  it('returns ok with userId=null if the session was already revoked / not found', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const result = await revokeSession(deps, {
      sessionId: SessionId('nope') as SessionIdType,
      reason: 'logout',
    });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.userId).toBeNull();
    }
  });
});

// -----------------------------------------------------------------------------
// enforceIdleAndAbsoluteTimeouts edge: equal-to-now is treated as expired
// -----------------------------------------------------------------------------

describe('enforceIdleAndAbsoluteTimeouts', () => {
  it('treats idleTimeoutAt === now as expired', async () => {
    const t0 = new Date('2026-01-01T00:00:00Z');
    const store = newStore();
    const crypto = mockCrypto();
    const deps = depsWithClock(t0, store, crypto);
    const userId = UserId('u1');
    const created = await createSession(deps, { userId, userAgent: null, remoteAddr: null });
    if (!created.ok) {throw new Error('setup');}

    const sess = created.value.session;
    // Make idleTimeoutAt exactly now.
    const sessAtIdle = { ...sess, idleTimeoutAt: t0 };
    const result = await enforceIdleAndAbsoluteTimeouts(deps, sessAtIdle);
    expect(result.ok).toBe(false);
  });
});

// Quiet vitest's `vi` unused-import warnings if any.
void vi;
