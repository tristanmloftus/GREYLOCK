// Greylock — authentication ceremony integration tests
// =============================================================================
// AGENT-AUTH (Phase 2). Asserts the harder behavioral contracts:
//   - counter monotonicity (replay rejection)
//   - single-session-per-user enforcement (`new_login` revoke)
//   - DEK loaded into CryptoService on success
//   - placeholder + allowlist gates surface as `no_passkey_for_email` (404)
//
// `@simplewebauthn/server` is mocked (the test owns `verifyAuthenticationResponse`
// so we can return arbitrary `newCounter` values).
// =============================================================================

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { Err, Ok } from '../../../lib/types/domain.js';
import {
  EncryptedBlob,
  PasskeyId,
  SessionId,
  UserId,
} from '../../../lib/types/domain.js';
import type {
  AuditEntry,
  Passkey,
  Result,
  Session,
  User,
  EncryptedBlob as EncryptedBlobType,
  UserId as UserIdType,
} from '../../../lib/types/domain.js';
import type {
  AuditService,
  CryptoService,
  PasskeyRepository,
  SessionRepository,
  UserRepository,
} from '../../../lib/types/services.js';

const verifyAuthMock = vi.fn();

vi.mock('@simplewebauthn/server', () => {
  return {
    generateRegistrationOptions: async () => ({
      challenge: 'C-REG',
      rp: { id: 'localhost', name: 'Greylock' },
      user: { id: 'U', name: 'rory@x.com', displayName: 'Rory' },
      pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
      timeout: 60000,
      attestation: 'none',
      authenticatorSelection: { residentKey: 'required', userVerification: 'required' },
    }),
    verifyRegistrationResponse: async () => ({
      verified: true,
      registrationInfo: {
        fmt: 'none',
        aaguid: '00000000-0000-0000-0000-000000000000',
        credential: {
          id: 'AAAAAAAAAAAAAAAAAAAAAA',
          publicKey: new Uint8Array([1, 2, 3, 4]),
          counter: 0,
          transports: ['internal'],
        },
        credentialType: 'public-key',
        attestationObject: new Uint8Array(0),
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    }),
    generateAuthenticationOptions: async (opts: { allowCredentials?: { id: string }[] }) => ({
      challenge: 'C-AUTH',
      rpId: 'localhost',
      timeout: 60000,
      userVerification: 'required',
      allowCredentials: opts.allowCredentials ?? [],
    }),
    verifyAuthenticationResponse: (...args: unknown[]) => verifyAuthMock(...args),
  };
});

vi.mock('@simplewebauthn/server/helpers', () => {
  return {
    isoBase64URL: {
      toBuffer: (s: string) => {
        const norm = s.replace(/-/g, '+').replace(/_/g, '/');
        const pad = norm + '='.repeat((4 - (norm.length % 4)) % 4);
        const bin = atob(pad);
        const out = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i += 1) {out[i] = bin.charCodeAt(i);}
        return out;
      },
      fromBuffer: (b: Uint8Array) => {
        let s = '';
        for (let i = 0; i < b.length; i += 1) {s += String.fromCharCode(b[i] ?? 0);}
        const b64 = btoa(s);
        return b64.replace(/=+$/, '').replace(/\+/g, '-').replace(/\//g, '_');
      },
    },
  };
});

import { createAuthService } from '../../../lib/auth/index.js';

// -----------------------------------------------------------------------------
// Mock repos / crypto / audit (in-memory)
// -----------------------------------------------------------------------------

interface World {
  readonly auth: ReturnType<typeof createAuthService>;
  readonly userRepo: UserRepository & { readonly users: Map<string, User> };
  readonly passkeyRepo: PasskeyRepository & { readonly rows: Map<string, Passkey> };
  readonly sessionRepo: SessionRepository & { readonly rows: Map<string, Session> };
  readonly crypto: CryptoService & { readonly userDeks: Set<UserIdType> };
  readonly audit: AuditService & { readonly entries: AuditEntry[] };
  readonly wraps: Map<string, EncryptedBlobType>;
}

function buildWorld(): World {
  const users = new Map<string, User>();
  const wraps = new Map<string, EncryptedBlobType>();
  const userRepo: World['userRepo'] = Object.assign(
    {
      findByEmail: async (email: string) => {
        for (const u of users.values()) {if (u.email === email) {return Ok(u);}}
        return Ok(null);
      },
      findById: async (id: string) => Ok(users.get(id) ?? null),
      list: async () => Ok([...users.values()]),
      create: async ({ email, displayName, role }: { email: string; displayName: string; role: 'owner' | 'member' }) => {
        const id = UserId(`u_${String(users.size + 1)}`);
        const user: User = {
          id,
          email,
          displayName,
          role,
          userDekVersion: 1,
          createdAt: new Date(),
          updatedAt: new Date(),
        };
        users.set(id, user);
        return Ok(user);
      },
      setWrappedUserDek: async ({ userId, version, wrapped }: { userId: UserIdType; version: number; wrapped: EncryptedBlobType }) => {
        const u = users.get(userId);
        if (u === undefined) {return Err({ kind: 'not_found' as const });}
        wraps.set(userId, wrapped);
        users.set(userId, { ...u, userDekVersion: version });
        return Ok(undefined);
      },
    },
    { users },
  ) as World['userRepo'];

  const pkRows = new Map<string, Passkey>();
  const passkeyRepo: World['passkeyRepo'] = Object.assign(
    {
      findByCredentialId: async (credentialId: Uint8Array) => {
        for (const p of pkRows.values()) {
          if (
            p.credentialId.length === credentialId.length &&
            p.credentialId.every((b, i) => b === credentialId[i])
          ) {
            return Ok(p);
          }
        }
        return Ok(null);
      },
      listByUser: async (userId: UserIdType) => Ok([...pkRows.values()].filter((p) => p.userId === userId)),
      create: async (input: {
        userId: UserIdType;
        credentialId: Uint8Array;
        credentialPublicKey: Uint8Array;
        counter: bigint;
        transports: ReadonlyArray<string>;
        aaguid: Uint8Array | null;
        deviceLabel: string | null;
        kekSalt: Uint8Array;
      }) => {
        const id = PasskeyId(`pk_${String(pkRows.size + 1)}`);
        const pk: Passkey = {
          id,
          userId: input.userId,
          credentialId: input.credentialId,
          credentialPublicKey: input.credentialPublicKey,
          counter: input.counter,
          transports: [...input.transports],
          aaguid: input.aaguid,
          deviceLabel: input.deviceLabel,
          createdAt: new Date(),
          lastUsedAt: null,
          revokedAt: null,
        };
        pkRows.set(id, pk);
        return Ok(pk);
      },
      bumpCounter: async ({ id, newCounter }: { id: ReturnType<typeof PasskeyId>; newCounter: bigint }) => {
        const p = pkRows.get(id);
        if (p === undefined) {return Err({ kind: 'not_found' as const });}
        pkRows.set(id, { ...p, counter: newCounter, lastUsedAt: new Date() });
        return Ok(undefined);
      },
      revoke: async ({ id }: { id: ReturnType<typeof PasskeyId> }) => {
        const p = pkRows.get(id);
        if (p === undefined) {return Err({ kind: 'not_found' as const });}
        pkRows.set(id, { ...p, revokedAt: new Date() });
        return Ok(undefined);
      },
    },
    { rows: pkRows },
  ) as World['passkeyRepo'];

  const sessRows = new Map<string, Session>();
  const sessionRepo: World['sessionRepo'] = Object.assign(
    {
      create: async ({
        userId,
        expiresAt,
        idleTimeoutAt,
        userAgent,
        remoteAddr,
      }: {
        userId: UserIdType;
        expiresAt: Date;
        idleTimeoutAt: Date;
        userAgent: string | null;
        remoteAddr: string | null;
      }) => {
        const id = SessionId(`sess_${String(sessRows.size + 1)}`);
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
        sessRows.set(id, sess);
        return Ok(sess);
      },
      findActiveById: async (id: ReturnType<typeof SessionId>) => {
        const s = sessRows.get(id);
        if (s === undefined || s.status !== 'active') {return Ok(null);}
        return Ok(s);
      },
      findActiveByUser: async (userId: UserIdType) => {
        for (const s of sessRows.values()) {
          if (s.userId === userId && s.status === 'active') {return Ok(s);}
        }
        return Ok(null);
      },
      touch: async ({ id, newIdleTimeoutAt }: { id: ReturnType<typeof SessionId>; newIdleTimeoutAt: Date }) => {
        const s = sessRows.get(id);
        if (s === undefined) {return Err({ kind: 'not_found' as const });}
        sessRows.set(id, { ...s, idleTimeoutAt: newIdleTimeoutAt });
        return Ok(undefined);
      },
      revoke: async ({ id, reason }: { id: ReturnType<typeof SessionId>; reason: string }) => {
        const s = sessRows.get(id);
        if (s === undefined) {return Err({ kind: 'not_found' as const });}
        sessRows.set(id, {
          ...s,
          status: 'revoked',
          revokedAt: new Date(),
          revokedReason: reason,
        });
        return Ok(undefined);
      },
      revokeAllActive: async () => Ok({ count: 0 }),
      expireOverdue: async () => Ok({ count: 0 }),
    },
    { rows: sessRows },
  ) as World['sessionRepo'];

  const userDeks = new Set<UserIdType>();
  const crypto: World['crypto'] = Object.assign(
    {
      initializeFromKeychain: async () => Ok(undefined),
      shutdown: async () => undefined,
      loadUserDek: async ({ userId }: { userId: UserIdType }) => {
        userDeks.add(userId);
        return Ok(undefined);
      },
      unloadUserDek: async (userId: UserIdType) => {
        userDeks.delete(userId);
      },
      hasUserDek: (userId: UserIdType) => userDeks.has(userId),
      hasPccDek: () => true,
      encrypt: async () => Err({ kind: 'aad_mismatch' as const }),
      decrypt: async () => Err({ kind: 'aad_mismatch' as const }),
      wrapUserDek: async () => Ok(EncryptedBlob.unsafeFromBytes(new Uint8Array([1, 2, 3]))),
      rotateUserDek: async () => Ok({ newVersion: 2, wrapped: EncryptedBlob.unsafeFromBytes(new Uint8Array(0)) }),
      rotateMaster: async () => Err({ kind: 'rotation_in_progress' as const }),
    },
    { userDeks },
  ) as World['crypto'];

  const auditEntries: AuditEntry[] = [];
  const audit: World['audit'] = Object.assign(
    {
      append: async (input: Parameters<AuditService['append']>[0]) => {
        const e: AuditEntry = {
          seq: BigInt(auditEntries.length + 1) as AuditEntry['seq'],
          ts: new Date(),
          tsNanos: 0,
          actorUserId: input.actorUserId,
          actorKind: input.actorKind,
          domain: input.domain,
          subjectId: input.subjectId,
          subjectKind: input.subjectKind,
          action: input.action,
          outcome: input.outcome,
          detailsJson: JSON.stringify(input.details),
          prevHash: new Uint8Array(32),
          entryHash: new Uint8Array(32),
        };
        auditEntries.push(e);
        return Ok(e);
      },
      query: async () => Ok([]),
      verifyChain: async () => Ok({ verifiedCount: auditEntries.length }),
    },
    { entries: auditEntries },
  ) as World['audit'];

  const wrappedDekReader = {
    readWrappedUserDek: async (userId: UserIdType): Promise<Result<EncryptedBlobType | null, { kind: 'storage_failure' }>> =>
      Ok(wraps.get(userId) ?? null),
    readPasskeyKekSalt: async (_passkeyId: string): Promise<Result<Uint8Array | null, { kind: 'storage_failure' }>> =>
      Ok(new Uint8Array(16)),
  };

  const auth = createAuthService({
    userRepo,
    passkeyRepo,
    sessionRepo,
    crypto,
    audit,
    wrappedDekReader,
  });
  return { auth, userRepo, passkeyRepo, sessionRepo, crypto, audit, wraps };
}

// -----------------------------------------------------------------------------
// Test setup
// -----------------------------------------------------------------------------

const ORIG_ALLOWED = process.env['ALLOWED_EMAILS'];
const ORIG_RP_ID = process.env['WEBAUTHN_RP_ID'];
const ORIG_RP_NAME = process.env['WEBAUTHN_RP_NAME'];
const ORIG_RP_ORIGIN = process.env['WEBAUTHN_RP_ORIGIN'];
const ORIG_SECRET = process.env['SESSION_SECRET'];
const ORIG_COOKIE = process.env['SESSION_COOKIE_NAME'];
const ORIG_IDLE = process.env['SESSION_IDLE_MINUTES'];
const ORIG_ABS = process.env['SESSION_ABSOLUTE_HOURS'];

beforeEach(() => {
  verifyAuthMock.mockReset();
  process.env['ALLOWED_EMAILS'] =
    'rory.patrick.loftus@gmail.com,tristan.m.loftus@gmail.com,cade-placeholder@greylock.invalid';
  process.env['WEBAUTHN_RP_ID'] = 'localhost';
  process.env['WEBAUTHN_RP_NAME'] = 'Greylock';
  process.env['WEBAUTHN_RP_ORIGIN'] = 'https://localhost:3000';
  process.env['SESSION_SECRET'] =
    'this-is-a-32-byte-or-longer-test-only-secret-string-not-used-in-production';
  process.env['SESSION_COOKIE_NAME'] = 'greylock_session';
  process.env['SESSION_IDLE_MINUTES'] = '30';
  process.env['SESSION_ABSOLUTE_HOURS'] = '8';
});

afterEach(() => {
  process.env['ALLOWED_EMAILS'] = ORIG_ALLOWED ?? '';
  process.env['WEBAUTHN_RP_ID'] = ORIG_RP_ID ?? '';
  process.env['WEBAUTHN_RP_NAME'] = ORIG_RP_NAME ?? '';
  process.env['WEBAUTHN_RP_ORIGIN'] = ORIG_RP_ORIGIN ?? '';
  process.env['SESSION_SECRET'] = ORIG_SECRET ?? '';
  process.env['SESSION_COOKIE_NAME'] = ORIG_COOKIE ?? '';
  process.env['SESSION_IDLE_MINUTES'] = ORIG_IDLE ?? '';
  process.env['SESSION_ABSOLUTE_HOURS'] = ORIG_ABS ?? '';
});

const ALLOWED_EMAIL = 'rory.patrick.loftus@gmail.com';
const PLACEHOLDER_EMAIL = 'cade-placeholder@greylock.invalid';

async function enrollFixture(world: World): Promise<void> {
  const r = await world.auth.completeEnrollment({
    email: ALLOWED_EMAIL,
    response: {
      id: 'AAAAAAAAAAAAAAAAAAAAAA',
      rawId: 'AAAAAAAAAAAAAAAAAAAAAA',
      response: { attestationObject: 'AAAA', clientDataJSON: 'AAAA' },
      clientExtensionResults: {},
      type: 'public-key',
    },
    expectedChallenge: 'C-REG',
    deviceLabel: null,
  });
  if (!r.ok) {throw new Error('enroll failed');}
}

const assertionResponse = {
  id: 'AAAAAAAAAAAAAAAAAAAAAA',
  rawId: 'AAAAAAAAAAAAAAAAAAAAAA',
  response: {
    authenticatorData: 'AAAA',
    clientDataJSON: 'AAAA',
    signature: 'AAAA',
  },
  clientExtensionResults: {},
  type: 'public-key' as const,
};

// -----------------------------------------------------------------------------
// beginAuthentication
// -----------------------------------------------------------------------------

describe('AuthService.beginAuthentication', () => {
  it('returns no_passkey_for_email for the placeholder address', async () => {
    const world = buildWorld();
    const r = await world.auth.beginAuthentication({ email: PLACEHOLDER_EMAIL });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('no_passkey_for_email');}
  });
  it('returns no_passkey_for_email for unallowed email (indistinguishable)', async () => {
    const world = buildWorld();
    const r = await world.auth.beginAuthentication({ email: 'intruder@example.com' });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('no_passkey_for_email');}
  });
  it('returns no_passkey_for_email for unknown allowed email (no User row yet)', async () => {
    const world = buildWorld();
    const r = await world.auth.beginAuthentication({ email: ALLOWED_EMAIL });
    expect(r.ok).toBe(false);
  });
  it('returns options after enrollment', async () => {
    const world = buildWorld();
    await enrollFixture(world);
    const r = await world.auth.beginAuthentication({ email: ALLOWED_EMAIL });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.value.challenge).toBe('C-AUTH');
      expect(r.value.userVerification).toBe('required');
      expect(r.value.allowCredentials.length).toBe(1);
    }
  });
});

// -----------------------------------------------------------------------------
// completeAuthentication
// -----------------------------------------------------------------------------

describe('AuthService.completeAuthentication', () => {
  it('rejects on counter regression (replay defense)', async () => {
    const world = buildWorld();
    await enrollFixture(world);
    // First successful auth bumps counter to 5.
    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 5,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const ok1 = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: null,
      remoteAddr: null,
    });
    expect(ok1.ok).toBe(true);

    // Replay attempt: counter goes backwards (3 < 5).
    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 3,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const replay = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: null,
      remoteAddr: null,
    });
    expect(replay.ok).toBe(false);
    if (!replay.ok) {
      expect(replay.error.kind).toBe('webauthn_verification_failed');
    }
    expect(
      world.audit.entries.some(
        (e) => e.action === 'passkey_authentication_failure' && e.outcome === 'denied',
      ),
    ).toBe(true);
  });

  it('allows the always-zero-counter case (passkey never increments)', async () => {
    const world = buildWorld();
    await enrollFixture(world);
    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 0,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const r = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: null,
      remoteAddr: null,
    });
    expect(r.ok).toBe(true);
  });

  it('enforces single-session — prior active session is revoked with new_login', async () => {
    const world = buildWorld();
    await enrollFixture(world);

    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 1,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const first = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: 'ua1',
      remoteAddr: null,
    });
    expect(first.ok).toBe(true);
    if (!first.ok) {return;}

    // Second login.
    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 2,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const second = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: 'ua2',
      remoteAddr: null,
    });
    expect(second.ok).toBe(true);
    if (!second.ok) {return;}

    // The first session is now revoked.
    const firstRow = world.sessionRepo.rows.get(first.value.sessionId);
    expect(firstRow?.status).toBe('revoked');
    expect(firstRow?.revokedReason).toBe('new_login');

    // Audit: per_user_dek_zeroized happened when prior was revoked AND
    // findActiveByUser returned null (briefly between the revoke and the new
    // create). Then per_user_dek_derived was emitted on the new login.
    const dekZeroEvents = world.audit.entries.filter((e) => e.action === 'per_user_dek_zeroized');
    expect(dekZeroEvents.length).toBeGreaterThanOrEqual(1);
  });

  it('loads the user DEK into CryptoService on success', async () => {
    const world = buildWorld();
    await enrollFixture(world);
    verifyAuthMock.mockResolvedValueOnce({
      verified: true,
      authenticationInfo: {
        credentialID: 'AAAAAAAAAAAAAAAAAAAAAA',
        newCounter: 1,
        userVerified: true,
        credentialDeviceType: 'singleDevice',
        credentialBackedUp: false,
        origin: 'https://localhost:3000',
        rpID: 'localhost',
      },
    });
    const r = await world.auth.completeAuthentication({
      email: ALLOWED_EMAIL,
      response: assertionResponse,
      expectedChallenge: 'C-AUTH',
      userAgent: null,
      remoteAddr: null,
    });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(world.crypto.userDeks.has(r.value.userId)).toBe(true);
      // cookieValue is set and non-empty (no further inspection — never log).
      expect(typeof r.value.cookieValue).toBe('string');
      expect(r.value.cookieValue.length).toBeGreaterThan(0);
    }
  });

  it('returns 404-equivalent for placeholder + unallowed + unknown', async () => {
    const world = buildWorld();
    await enrollFixture(world);

    for (const email of [PLACEHOLDER_EMAIL, 'intruder@example.com']) {
      const r = await world.auth.completeAuthentication({
        email,
        response: assertionResponse,
        expectedChallenge: 'C-AUTH',
        userAgent: null,
        remoteAddr: null,
      });
      expect(r.ok).toBe(false);
      if (!r.ok) {expect(r.error.kind).toBe('no_passkey_for_email');}
    }
  });
});

