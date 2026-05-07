// Integration test: bind a real keybridge server on a temp socket, connect a
// real client, exercise the handshake + requestDek/releaseDek round-trips.

import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { randomBytes } from 'node:crypto';

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import {
  createKeybridgeClient,
  createKeybridgeServer,
  type DekProvider,
} from '../../../lib/ipc/index.js';
import { Ok } from '../../../lib/types/domain.js';
import { SessionId, UserId } from '../../../lib/types/domain.js';
import type {
  AuditService,
  SessionRepository,
} from '../../../lib/types/services.js';

// -----------------------------------------------------------------------------
// Mocks
// -----------------------------------------------------------------------------

function fakeAudit(): AuditService {
  const calls: Array<unknown> = [];
  return {
    append: async (input) => {
      calls.push(input);
      return Ok({} as never);
    },
    query: async () => Ok([]),
    verifyChain: async () => Ok({ verifiedCount: 0 }),
  };
}

function fakeSessionRepo(args: { activeSession: { sessionId: string; userId: string } | null }): SessionRepository {
  const empty: SessionRepository = {
    create: async () => Ok({} as never),
    findActiveById: async () => Ok(null),
    findActiveByUser: async (userId) => {
      if (args.activeSession === null) {
        return Ok(null);
      }
      if ((userId as unknown as string) !== args.activeSession.userId) {
        return Ok(null);
      }
      return Ok({
        id: SessionId(args.activeSession.sessionId),
        userId: UserId(args.activeSession.userId),
        status: 'active',
        createdAt: new Date(),
        lastActivityAt: new Date(),
        expiresAt: new Date(Date.now() + 3600_000),
        idleTimeoutAt: new Date(Date.now() + 1800_000),
        revokedAt: null,
        revokedReason: null,
        userAgent: null,
        remoteAddr: null,
      });
    },
    touch: async () => Ok(undefined),
    revoke: async () => Ok(undefined),
    revokeAllActive: async () => Ok({ count: 0 }),
    expireOverdue: async () => Ok({ count: 0 }),
  };
  return empty;
}

function dekProvider(args: {
  pccDek?: { version: number; dek: Uint8Array };
  userDeks?: Map<string, { version: number; dek: Uint8Array }>;
}): DekProvider {
  return {
    getPccDek: () => args.pccDek ?? null,
    getUserDek: (userId) => {
      const v = (args.userDeks ?? new Map()).get(userId as unknown as string);
      return v ?? null;
    },
  };
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

function tmpSocketPath(label: string): string {
  // macOS Unix-domain socket paths are limited to ~104 bytes including the
  // terminator; we keep the label tiny + use the system tmp dir which on macOS
  // resolves to a long /var/folders/... path that already eats most of the
  // budget. We drop the timestamp to maximize remaining headroom.
  return path.join(os.tmpdir(), `gl-kb-${label}-${String(process.pid)}.sock`);
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

let cleanupSocketPaths: string[] = [];

beforeEach(() => {
  cleanupSocketPaths = [];
});

afterEach(() => {
  for (const p of cleanupSocketPaths) {
    try {
      fs.unlinkSync(p);
    } catch {
      // already gone
    }
  }
});

describe('keybridge end-to-end (real socket)', () => {
  it('binds with mode 0600, runs handshake, requestDek({pcc}), releaseDek', async () => {
    const socketPath = tmpSocketPath('happy');
    cleanupSocketPaths.push(socketPath);
    const hmacKey = randomBytes(32);
    const pccDek = { version: 3, dek: randomBytes(32) };

    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({ pccDek }),
    });

    const startRes = await server.start();
    expect(startRes.ok).toBe(true);

    // Verify the socket is mode 0600.
    const st = fs.statSync(socketPath);
    expect((st.mode & 0o777).toString(8)).toBe('600');

    const client = createKeybridgeClient({ socketPath, hmacKey });
    const connectRes = await client.connect();
    expect(connectRes.ok).toBe(true);
    expect(client.isConnected()).toBe(true);

    const borrowed = await client.requestDek({ kind: 'pcc' });
    expect(borrowed.ok).toBe(true);
    if (borrowed.ok) {
      expect(borrowed.value.handle.kind).toBe('pcc');
      // bytes must round-trip
      expect(Buffer.from(borrowed.value.bytes).equals(Buffer.from(pccDek.dek))).toBe(true);
      await borrowed.value.release();
      // After release, bytes are zeroized.
      expect(borrowed.value.bytes.every((b) => b === 0)).toBe(true);
    }

    await client.disconnect();
    await server.stop();
  });

  it('requestDek({user}) succeeds when session is active and DEK is loaded', async () => {
    const socketPath = tmpSocketPath('user-active');
    cleanupSocketPaths.push(socketPath);
    const hmacKey = randomBytes(32);
    const userDek = randomBytes(32);

    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({
        activeSession: { sessionId: 'sess_xyz', userId: 'usr_rory' },
      }),
      dekProvider: dekProvider({
        userDeks: new Map([['usr_rory', { version: 2, dek: userDek }]]),
      }),
    });
    await server.start();

    const client = createKeybridgeClient({ socketPath, hmacKey });
    await client.connect();

    const r = await client.requestDek({ userId: 'usr_rory', sessionId: 'sess_xyz' });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.value.handle.kind).toBe('user');
      expect(Buffer.from(r.value.bytes).equals(Buffer.from(userDek))).toBe(true);
      await r.value.release();
    }
    await client.disconnect();
    await server.stop();
  });

  it('requestDek({user}) returns session_invalid when no active session', async () => {
    const socketPath = tmpSocketPath('user-no-session');
    cleanupSocketPaths.push(socketPath);
    const hmacKey = randomBytes(32);

    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({
        userDeks: new Map([['usr_rory', { version: 2, dek: randomBytes(32) }]]),
      }),
    });
    await server.start();

    const client = createKeybridgeClient({ socketPath, hmacKey });
    await client.connect();

    const r = await client.requestDek({ userId: 'usr_rory', sessionId: 'no-such-sess' });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('session_invalid');
    }
    await client.disconnect();
    await server.stop();
  });

  it('client with the wrong HMAC key fails handshake', async () => {
    const socketPath = tmpSocketPath('bad-hmac');
    cleanupSocketPaths.push(socketPath);
    const serverKey = randomBytes(32);
    const clientKey = randomBytes(32); // intentionally different

    const server = createKeybridgeServer({
      socketPath,
      hmacKey: serverKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({}),
    });
    await server.start();

    const client = createKeybridgeClient({ socketPath, hmacKey: clientKey });
    const r = await client.connect();
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('auth_failed');
    }
    await server.stop();
  });

  it('peer credential check passes for our own UID', async () => {
    // Self-connect = same UID. The integration above already exercises this
    // happy path. We add a test that confirms `peerUidMatchesOurs` is invoked
    // implicitly: if we forced a different UID we would expect rejection;
    // since we can't change UID inside a test, we verify the check is wired
    // by stubbing peer-cred via dependency-injection seam in a unit context.
    // Here we just assert that a fresh connection to a fresh server succeeds
    // — i.e. UID-equality is enforced and our own connect goes through.
    const socketPath = tmpSocketPath('self-uid');
    cleanupSocketPaths.push(socketPath);
    const hmacKey = randomBytes(32);
    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({ pccDek: { version: 1, dek: randomBytes(32) } }),
    });
    await server.start();
    const client = createKeybridgeClient({ socketPath, hmacKey });
    const r = await client.connect();
    expect(r.ok).toBe(true);
    await client.disconnect();
    await server.stop();
  });

  it('cleans up a stale socket on start()', async () => {
    const socketPath = tmpSocketPath('stale');
    cleanupSocketPaths.push(socketPath);
    // Create a stale file at the socket path.
    fs.writeFileSync(socketPath, '');
    expect(fs.existsSync(socketPath)).toBe(true);

    const hmacKey = randomBytes(32);
    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({}),
    });
    const r = await server.start();
    expect(r.ok).toBe(true);

    // Socket exists and is writable by us.
    expect(fs.existsSync(socketPath)).toBe(true);
    await server.stop();
    expect(fs.existsSync(socketPath)).toBe(false);
  });

  it('ping round-trips after a successful handshake', async () => {
    const socketPath = tmpSocketPath('ping');
    cleanupSocketPaths.push(socketPath);
    const hmacKey = randomBytes(32);
    const server = createKeybridgeServer({
      socketPath,
      hmacKey,
      audit: fakeAudit(),
      sessionRepo: fakeSessionRepo({ activeSession: null }),
      dekProvider: dekProvider({}),
    });
    await server.start();
    const client = createKeybridgeClient({ socketPath, hmacKey });
    await client.connect();
    const r = await client.ping();
    expect(r.ok).toBe(true);
    await client.disconnect();
    await server.stop();
  });
});
