// Greylock — registration ceremony integration tests
// =============================================================================
// AGENT-AUTH (Phase 2). Exercises the AuthService factory end-to-end with
// `@simplewebauthn/server` mocked at the module boundary so tests don't have
// to forge real attestation objects. Repos and CryptoService are in-memory
// stubs; the test asserts the *behavioral contract* (allowlist, placeholder,
// User+Passkey persistence, wrappedUserDek populated, audit emits).
// =============================================================================

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { Err, Ok } from '../../../lib/types/domain.js';
import {
  EncryptedBlob,
  PasskeyId,
  UserId,
} from '../../../lib/types/domain.js';
import type {
  AuditEntry,
  Passkey,
  Result,
  Session,
  User,
  UserId as UserIdType,
  EncryptedBlob as EncryptedBlobType,
} from '../../../lib/types/domain.js';
import type {
  AuditService,
  CryptoService,
  PasskeyRepository,
  SessionRepository,
  UserRepository,
} from '../../../lib/types/services.js';

// IMPORTANT: vi.mock is hoisted; the factory must be self-contained.
vi.mock('@simplewebauthn/server', () => {
  return {
    generateRegistrationOptions: async (opts: { rpID: string; rpName: string; userName: string }) => ({
      challenge: 'CHALLENGE-REG',
      rp: { id: opts.rpID, name: opts.rpName },
      user: { id: 'USER-ID-B64', name: opts.userName, displayName: opts.userName },
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
    generateAuthenticationOptions: async () => ({
      challenge: 'CHALLENGE-AUTH',
      rpId: 'localhost',
      timeout: 60000,
      userVerification: 'required',
      allowCredentials: [],
    }),
    verifyAuthenticationResponse: async () => ({
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
    }),
  };
});

vi.mock('@simplewebauthn/server/helpers', () => {
  return {
    isoBase64URL: {
      toBuffer: (s: string) => {
        // tiny base64url decoder for fixture strings
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

// The webauthn wrapper imports from `@simplewebauthn/server/esm/deps.js` for
// types only — no need to mock that.

// Now we can import the auth service factory.
import { createAuthService } from '../../../lib/auth/index.js';

// -----------------------------------------------------------------------------
// In-memory mock repos / crypto / audit
// -----------------------------------------------------------------------------

function mockUserRepo(): UserRepository & {
  readonly users: Map<string, User>;
  readonly wraps: Map<string, EncryptedBlobType>;
} {
  const users = new Map<string, User>();
  const wraps = new Map<string, EncryptedBlobType>();
  return {
    users,
    wraps,
    findByEmail: async (email) => {
      for (const u of users.values()) {
        if (u.email === email) {return Ok(u);}
      }
      return Ok(null);
    },
    findById: async (id) => Ok(users.get(id) ?? null),
    list: async () => Ok([...users.values()]),
    create: async ({ email, displayName, role }) => {
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
    setWrappedUserDek: async ({ userId, version, wrapped }) => {
      const u = users.get(userId);
      if (u === undefined) {return Err({ kind: 'not_found' });}
      wraps.set(userId, wrapped);
      users.set(userId, { ...u, userDekVersion: version });
      // Mirror onto the user record for the auth flow's consumption.
      // (Domain User has wrappedUserDek? Inspect domain.ts: User does NOT have
      // wrappedUserDek. The Prisma row does, though, and the AuthService
      // expects User to expose it. We therefore augment via a side-channel
      // that the auth service reads.)
      return Ok(undefined);
    },
  };
}

function mockPasskeyRepo(): PasskeyRepository & { readonly rows: Map<string, Passkey> } {
  const rows = new Map<string, Passkey>();
  return {
    rows,
    findByCredentialId: async (credentialId) => {
      for (const p of rows.values()) {
        if (
          p.credentialId.length === credentialId.length &&
          p.credentialId.every((b, i) => b === credentialId[i])
        ) {
          return Ok(p);
        }
      }
      return Ok(null);
    },
    listByUser: async (userId) => Ok([...rows.values()].filter((p) => p.userId === userId)),
    create: async (input) => {
      const id = PasskeyId(`pk_${String(rows.size + 1)}`);
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
      rows.set(id, pk);
      return Ok(pk);
    },
    bumpCounter: async ({ id, newCounter }) => {
      const p = rows.get(id);
      if (p === undefined) {return Err({ kind: 'not_found' });}
      rows.set(id, { ...p, counter: newCounter, lastUsedAt: new Date() });
      return Ok(undefined);
    },
    revoke: async ({ id }) => {
      const p = rows.get(id);
      if (p === undefined) {return Err({ kind: 'not_found' });}
      rows.set(id, { ...p, revokedAt: new Date() });
      return Ok(undefined);
    },
  };
}

function mockSessionRepo(): SessionRepository & { readonly rows: Map<string, Session> } {
  const rows = new Map<string, Session>();
  return {
    rows,
    create: async ({ userId, expiresAt, idleTimeoutAt, userAgent, remoteAddr }) => {
      const id = `sess_${String(rows.size + 1)}` as Session['id'];
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
      rows.set(id, sess);
      return Ok(sess);
    },
    findActiveById: async (id) => {
      const r = rows.get(id);
      if (r === undefined || r.status !== 'active') {return Ok(null);}
      return Ok(r);
    },
    findActiveByUser: async (userId) => {
      for (const s of rows.values()) {
        if (s.userId === userId && s.status === 'active') {return Ok(s);}
      }
      return Ok(null);
    },
    touch: async ({ id, newIdleTimeoutAt }) => {
      const s = rows.get(id);
      if (s === undefined) {return Err({ kind: 'not_found' });}
      rows.set(id, { ...s, idleTimeoutAt: newIdleTimeoutAt });
      return Ok(undefined);
    },
    revoke: async ({ id, reason }) => {
      const s = rows.get(id);
      if (s === undefined) {return Err({ kind: 'not_found' });}
      rows.set(id, {
        ...s,
        status: 'revoked',
        revokedAt: new Date(),
        revokedReason: reason,
      });
      return Ok(undefined);
    },
    revokeAllActive: async () => Ok({ count: 0 }),
    expireOverdue: async () => Ok({ count: 0 }),
  };
}

type MockCrypto = CryptoService & {
  readonly userDeks: Set<UserIdType>;
  wrapsCalled: number;
};

function mockCrypto(): MockCrypto {
  const userDeks = new Set<UserIdType>();
  const out = {
    userDeks,
    wrapsCalled: 0,
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
    wrapUserDek: async () => {
      out.wrapsCalled += 1;
      return Ok(EncryptedBlob.unsafeFromBytes(new Uint8Array([0xff, 0xee, 0xdd])));
    },
    rotateUserDek: async () =>
      Ok({ newVersion: 2, wrapped: EncryptedBlob.unsafeFromBytes(new Uint8Array(0)) }),
    rotateMaster: async () => Err({ kind: 'rotation_in_progress' as const }),
  };
  return out as MockCrypto;
}

function mockAudit(): AuditService & { readonly entries: AuditEntry[] } {
  const entries: AuditEntry[] = [];
  return {
    entries,
    append: async (input) => {
      const e: AuditEntry = {
        seq: BigInt(entries.length + 1) as AuditEntry['seq'],
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
      entries.push(e);
      return Ok(e);
    },
    query: async () => Ok([]),
    verifyChain: async () => Ok({ verifiedCount: entries.length }),
  };
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

beforeEach(() => {
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
  if (ORIG_ALLOWED === undefined) {delete process.env['ALLOWED_EMAILS'];}
  else {process.env['ALLOWED_EMAILS'] = ORIG_ALLOWED;}
  if (ORIG_RP_ID === undefined) {delete process.env['WEBAUTHN_RP_ID'];}
  else {process.env['WEBAUTHN_RP_ID'] = ORIG_RP_ID;}
  if (ORIG_RP_NAME === undefined) {delete process.env['WEBAUTHN_RP_NAME'];}
  else {process.env['WEBAUTHN_RP_NAME'] = ORIG_RP_NAME;}
  if (ORIG_RP_ORIGIN === undefined) {delete process.env['WEBAUTHN_RP_ORIGIN'];}
  else {process.env['WEBAUTHN_RP_ORIGIN'] = ORIG_RP_ORIGIN;}
  if (ORIG_SECRET === undefined) {delete process.env['SESSION_SECRET'];}
  else {process.env['SESSION_SECRET'] = ORIG_SECRET;}
  if (ORIG_COOKIE === undefined) {delete process.env['SESSION_COOKIE_NAME'];}
  else {process.env['SESSION_COOKIE_NAME'] = ORIG_COOKIE;}
});

const ALLOWED_EMAIL = 'rory.patrick.loftus@gmail.com';
const PLACEHOLDER_EMAIL = 'cade-placeholder@greylock.invalid';

function buildSvc() {
  const userRepo = mockUserRepo();
  const passkeyRepo = mockPasskeyRepo();
  const sessionRepo = mockSessionRepo();
  const crypto = mockCrypto();
  const audit = mockAudit();
  const passkeySalts = new Map<string, Uint8Array>();
  const wrappedDekReader = {
    readWrappedUserDek: async (userId: UserIdType): Promise<Result<EncryptedBlobType | null, { kind: 'storage_failure' }>> => {
      const w = userRepo.wraps.get(userId);
      return Ok(w ?? null);
    },
    readPasskeyKekSalt: async (passkeyId: string): Promise<Result<Uint8Array | null, { kind: 'storage_failure' }>> => {
      return Ok(passkeySalts.get(passkeyId) ?? new Uint8Array(16));
    },
  };
  const auth = createAuthService({
    userRepo,
    passkeyRepo,
    sessionRepo,
    crypto,
    audit,
    wrappedDekReader,
  });
  return { auth, userRepo, passkeyRepo, sessionRepo, crypto, audit, wrappedDekReader };
}

// -----------------------------------------------------------------------------
// beginEnrollment
// -----------------------------------------------------------------------------

describe('AuthService.beginEnrollment', () => {
  it('rejects the placeholder email even when listed in ALLOWED_EMAILS', async () => {
    const { auth, audit } = buildSvc();
    const r = await auth.beginEnrollment({
      email: PLACEHOLDER_EMAIL,
      displayName: 'Cade',
      role: 'member',
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('placeholder_email_rejected');}
    expect(audit.entries.some((e) => e.action === 'passkey_enrollment_rejected' && e.outcome === 'denied')).toBe(true);
  });

  it('rejects an email not in the allowlist', async () => {
    const { auth, audit } = buildSvc();
    const r = await auth.beginEnrollment({
      email: 'intruder@example.com',
      displayName: 'I',
      role: 'member',
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('email_not_allowlisted');}
    expect(audit.entries.some((e) => e.action === 'passkey_enrollment_rejected')).toBe(true);
  });

  it('returns options for an allowed email', async () => {
    const { auth } = buildSvc();
    const r = await auth.beginEnrollment({
      email: ALLOWED_EMAIL,
      displayName: 'Rory',
      role: 'owner',
    });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.value.challenge).toBe('CHALLENGE-REG');
      expect(r.value.attestation).toBe('none');
      expect(r.value.authenticatorSelection.residentKey).toBe('required');
      expect(r.value.authenticatorSelection.userVerification).toBe('required');
    }
  });
});

// -----------------------------------------------------------------------------
// completeEnrollment
// -----------------------------------------------------------------------------

describe('AuthService.completeEnrollment', () => {
  it('persists User + Passkey and writes wrappedUserDek', async () => {
    const { auth, userRepo, passkeyRepo, crypto } = buildSvc();
    const r = await auth.completeEnrollment({
      email: ALLOWED_EMAIL,
      response: {
        id: 'AAAAAAAAAAAAAAAAAAAAAA',
        rawId: 'AAAAAAAAAAAAAAAAAAAAAA',
        response: {
          attestationObject: 'AAAA',
          clientDataJSON: 'AAAA',
        },
        clientExtensionResults: {},
        type: 'public-key',
      },
      expectedChallenge: 'CHALLENGE-REG',
      deviceLabel: "Rory's iPhone",
    });
    expect(r.ok).toBe(true);
    if (!r.ok) {return;}
    expect(userRepo.users.size).toBe(1);
    expect(passkeyRepo.rows.size).toBe(1);
    expect(userRepo.wraps.has(r.value.userId)).toBe(true);
    expect(crypto.wrapsCalled).toBe(1);
  });

  it('rejects placeholder email at completeEnrollment too', async () => {
    const { auth } = buildSvc();
    const r = await auth.completeEnrollment({
      email: PLACEHOLDER_EMAIL,
      response: {
        id: 'A',
        rawId: 'A',
        response: { attestationObject: 'A', clientDataJSON: 'A' },
        clientExtensionResults: {},
        type: 'public-key',
      },
      expectedChallenge: 'X',
      deviceLabel: null,
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('placeholder_email_rejected');}
  });

  it('rejects unallowlisted email at completeEnrollment too', async () => {
    const { auth } = buildSvc();
    const r = await auth.completeEnrollment({
      email: 'intruder@example.com',
      response: {
        id: 'A',
        rawId: 'A',
        response: { attestationObject: 'A', clientDataJSON: 'A' },
        clientExtensionResults: {},
        type: 'public-key',
      },
      expectedChallenge: 'X',
      deviceLabel: null,
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {expect(r.error.kind).toBe('email_not_allowlisted');}
  });

  it('returns passkey_already_enrolled if a non-revoked passkey exists', async () => {
    const { auth } = buildSvc();
    // First enrollment
    const r1 = await auth.completeEnrollment({
      email: ALLOWED_EMAIL,
      response: {
        id: 'AAAAAAAAAAAAAAAAAAAAAA',
        rawId: 'AAAAAAAAAAAAAAAAAAAAAA',
        response: { attestationObject: 'A', clientDataJSON: 'A' },
        clientExtensionResults: {},
        type: 'public-key',
      },
      expectedChallenge: 'CHALLENGE-REG',
      deviceLabel: null,
    });
    expect(r1.ok).toBe(true);
    // Second attempt should fail.
    const r2 = await auth.completeEnrollment({
      email: ALLOWED_EMAIL,
      response: {
        id: 'AAAAAAAAAAAAAAAAAAAAAA',
        rawId: 'AAAAAAAAAAAAAAAAAAAAAA',
        response: { attestationObject: 'A', clientDataJSON: 'A' },
        clientExtensionResults: {},
        type: 'public-key',
      },
      expectedChallenge: 'CHALLENGE-REG',
      deviceLabel: null,
    });
    expect(r2.ok).toBe(false);
    if (!r2.ok) {expect(r2.error.kind).toBe('passkey_already_enrolled');}
  });
});

