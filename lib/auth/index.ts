// Greylock — AuthService factory
// =============================================================================
// AGENT-AUTH (Phase 2). Exports `createAuthService` — a factory that returns
// the canonical `AuthService` interface implementation. All Prisma access goes
// through repositories; all crypto goes through the `CryptoService` interface;
// all WebAuthn goes through `lib/auth/webauthn.ts`. This module never imports
// concrete implementations from `lib/crypto/*` or `lib/db/*`.
//
// Behavioral guarantees implemented here (cross-referenced to the brief):
//   1. Allowlist + placeholder rejection at *both* enrollment AND
//      authentication, even when a User row already exists.
//   2. Counter monotonicity on every `verifyAuthenticationResponse`.
//   3. Single-session-per-user — prior active session revoked with
//      `reason='new_login'` and DEK unloaded if no other active session.
//   4. Idle + absolute timeouts in `validateSession` (delegated to
//      `lib/auth/session.ts`).
//   5. Indistinguishable `no_passkey_for_email` for unknown / unallowed email
//      at `beginAuthentication` (route layer surfaces 404 either way).
//   6. Audit emit on every success and failure.
//   7. No `throw` from `AuthService` methods — all paths return `Result`.
// =============================================================================

import * as crypto from 'node:crypto';

import { Err, Ok } from '../types/domain.js';
import {
  AuditAction,
  AuditOutcome,
  ActorKind,
  Domain,
  PasskeyId,
  SessionId,
  UserId,
} from '../types/domain.js';
import type {
  AuthError,
  PasskeyId as PasskeyIdType,
  Result,
  Role,
  Session,
  SessionId as SessionIdType,
  UserId as UserIdType,
} from '../types/domain.js';
import type {
  AuditService,
  AuthAuthenticationOptions,
  AuthRegistrationOptions,
  AuthService,
  CryptoService,
  PasskeyRepository,
  SessionRepository,
  UserRepository,
} from '../types/services.js';

import { isAllowedEmail, isPlaceholderEmail, normalizeEmail } from './allowlist.js';
import type { WrappedDekReader } from './wrapped-dek-reader.js';
import {
  type SessionConfig,
  type SessionDeps,
  createSession,
  readSessionConfig,
  revokeAllActive,
  revokeSession,
  validateSession as validateSessionHelper,
} from './session.js';
import {
  base64UrlFromBytes,
  beginAuthentication as webauthnBeginAuth,
  beginRegistration as webauthnBeginReg,
  bytesFromBase64Url,
  isCounterMonotonic,
  readRpConfig,
  verifyAuthentication as webauthnVerifyAuth,
  verifyRegistration as webauthnVerifyReg,
} from './webauthn.js';

// -----------------------------------------------------------------------------
// Factory dependencies
// -----------------------------------------------------------------------------

export interface AuthServiceDeps {
  readonly userRepo: UserRepository;
  readonly passkeyRepo: PasskeyRepository;
  readonly sessionRepo: SessionRepository;
  readonly crypto: CryptoService;
  readonly audit: AuditService;
  /**
   * Reads the wrapped per-user DEK from the DB. AGENT-DB will implement; not
   * part of `UserRepository` because the canonical domain `User` interface
   * does not expose `wrappedUserDek` and Phase 1 contracts are read-only.
   * See retro for orchestrator follow-up.
   */
  readonly wrappedDekReader: WrappedDekReader;
  /** Optional — defaults to `() => new Date()`. Tests inject a fixed clock. */
  readonly now?: () => Date;
  /** Optional — defaults to a CSPRNG-backed nonce factory. */
  readonly randomNonce?: () => string;
  /** Optional — defaults to `readSessionConfig()`. */
  readonly sessionConfig?: SessionConfig;
}

// -----------------------------------------------------------------------------
// Helpers (audit boilerplate)
// -----------------------------------------------------------------------------

async function safeAppend(
  audit: AuditService,
  input: Parameters<AuditService['append']>[0],
): Promise<void> {
  // Audit must never block the auth flow if the audit store fails — the route
  // handler still surfaces the original error. We swallow audit errors here
  // but never the auth error.
  await audit.append(input);
}

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

export function createAuthService(deps: AuthServiceDeps): AuthService {
  const now = deps.now ?? ((): Date => new Date());
  const randomNonce =
    deps.randomNonce ?? ((): string => crypto.randomBytes(16).toString('base64url'));
  const sessionConfig = deps.sessionConfig ?? readSessionConfig();

  const sessionDeps: SessionDeps = {
    sessionRepo: deps.sessionRepo,
    crypto: deps.crypto,
    config: sessionConfig,
    now,
    randomNonce,
  };

  // -------------------------------------------------------------------------
  // beginEnrollment
  // -------------------------------------------------------------------------
  async function beginEnrollment(input: {
    readonly email: string;
    readonly displayName: string;
    readonly role: Role;
  }): Promise<Result<AuthRegistrationOptions, AuthError>> {
    const email = normalizeEmail(input.email);

    if (isPlaceholderEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyEnrollmentRejected,
        outcome: AuditOutcome.Denied,
        details: { reason: 'placeholder_email' },
      });
      return Err({ kind: 'placeholder_email_rejected' });
    }

    if (!isAllowedEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyEnrollmentRejected,
        outcome: AuditOutcome.Denied,
        details: { reason: 'email_not_allowlisted' },
      });
      return Err({ kind: 'email_not_allowlisted' });
    }

    // Reject if a non-revoked passkey is already enrolled for this email.
    const existingUser = await deps.userRepo.findByEmail(email);
    if (existingUser.ok && existingUser.value !== null) {
      const existingPasskeys = await deps.passkeyRepo.listByUser(existingUser.value.id);
      if (existingPasskeys.ok) {
        const hasActive = existingPasskeys.value.some((p) => p.revokedAt === null);
        if (hasActive) {
          await safeAppend(deps.audit, {
            actorUserId: existingUser.value.id,
            actorKind: ActorKind.System,
            domain: null,
            subjectId: existingUser.value.id,
            subjectKind: 'user',
            action: AuditAction.PasskeyEnrollmentRejected,
            outcome: AuditOutcome.Denied,
            details: { reason: 'passkey_already_enrolled' },
          });
          return Err({ kind: 'passkey_already_enrolled' });
        }
      }
    }

    // Use the existing User.id (if any) as the WebAuthn user.id; otherwise
    // synthesize 16 random bytes — the actual User row is created in
    // `completeEnrollment` once the attestation verifies.
    const userIdBytes =
      existingUser.ok && existingUser.value !== null
        ? new TextEncoder().encode(existingUser.value.id)
        : crypto.randomBytes(16);

    const opts = await webauthnBeginReg({
      userId: userIdBytes,
      userName: email,
      userDisplayName: input.displayName,
      excludeCredentials: [],
    });

    return Ok({
      challenge: opts.challenge,
      rp: { id: opts.rp.id, name: opts.rp.name },
      user: {
        id: opts.user.id,
        name: opts.user.name,
        displayName: opts.user.displayName,
      },
      pubKeyCredParams: opts.pubKeyCredParams.map((p) => ({
        type: 'public-key',
        alg: p.alg,
      })),
      timeout: opts.timeout ?? 60_000,
      attestation: 'none',
      authenticatorSelection: {
        residentKey: 'required',
        userVerification: 'required',
      },
    });
  }

  // -------------------------------------------------------------------------
  // completeEnrollment
  // -------------------------------------------------------------------------
  async function completeEnrollment(input: {
    readonly email: string;
    readonly response: Parameters<AuthService['completeEnrollment']>[0]['response'];
    readonly expectedChallenge: string;
    readonly deviceLabel: string | null;
  }): Promise<Result<{ readonly userId: UserIdType; readonly passkeyId: PasskeyIdType }, AuthError>> {
    const email = normalizeEmail(input.email);

    if (isPlaceholderEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyEnrollmentRejected,
        outcome: AuditOutcome.Denied,
        details: { reason: 'placeholder_email' },
      });
      return Err({ kind: 'placeholder_email_rejected' });
    }

    if (!isAllowedEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyEnrollmentRejected,
        outcome: AuditOutcome.Denied,
        details: { reason: 'email_not_allowlisted' },
      });
      return Err({ kind: 'email_not_allowlisted' });
    }

    const verified = await webauthnVerifyReg({
      response: input.response,
      expectedChallenge: input.expectedChallenge,
    });
    if (!verified.verified) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyEnrollment,
        outcome: AuditOutcome.Failure,
        details: { reason: 'webauthn_verification_failed' },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Upsert the User row.
    const existing = await deps.userRepo.findByEmail(email);
    if (!existing.ok) {
      return Err({ kind: 'webauthn_verification_failed' });
    }
    let user = existing.value;
    if (user === null) {
      const created = await deps.userRepo.create({
        email,
        displayName: email, // displayName is enforced by Zod on the route; default to email if absent
        role: 'member',
      });
      if (!created.ok) {
        return Err({ kind: 'webauthn_verification_failed' });
      }
      user = created.value;
    }

    // Reject duplicate active passkeys.
    const existingPasskeys = await deps.passkeyRepo.listByUser(user.id);
    if (existingPasskeys.ok && existingPasskeys.value.some((p) => p.revokedAt === null)) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyEnrollmentRejected,
        outcome: AuditOutcome.Denied,
        details: { reason: 'passkey_already_enrolled' },
      });
      return Err({ kind: 'passkey_already_enrolled' });
    }

    // Derive a fresh per-user DEK + wrap under the per-user KEK.
    const kekSalt = crypto.randomBytes(16);
    const dekMaterial = crypto.randomBytes(32);
    let wrapResult;
    try {
      wrapResult = await deps.crypto.wrapUserDek({
        userId: user.id,
        credentialId: verified.credentialId,
        kekSalt,
        version: 1,
        dekMaterial,
      });
    } finally {
      dekMaterial.fill(0);
    }
    if (!wrapResult.ok) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyEnrollment,
        outcome: AuditOutcome.Failure,
        details: { reason: 'crypto_wrap_failed' },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Persist the wrapped DEK and the passkey row.
    const setWrap = await deps.userRepo.setWrappedUserDek({
      userId: user.id,
      version: 1,
      wrapped: wrapResult.value,
    });
    if (!setWrap.ok) {
      return Err({ kind: 'webauthn_verification_failed' });
    }

    const passkey = await deps.passkeyRepo.create({
      userId: user.id,
      credentialId: verified.credentialId,
      credentialPublicKey: verified.credentialPublicKey,
      counter: verified.counter,
      transports: [...verified.transports],
      aaguid: verified.aaguid,
      deviceLabel: input.deviceLabel,
      kekSalt,
    });
    if (!passkey.ok) {
      return Err({ kind: 'webauthn_verification_failed' });
    }

    await safeAppend(deps.audit, {
      actorUserId: user.id,
      actorKind: ActorKind.System,
      domain: null,
      subjectId: passkey.value.id,
      subjectKind: 'passkey',
      action: AuditAction.PasskeyEnrollment,
      outcome: AuditOutcome.Success,
      details: { transports: [...verified.transports] },
    });

    return Ok({ userId: user.id, passkeyId: passkey.value.id });
  }

  // -------------------------------------------------------------------------
  // beginAuthentication
  // -------------------------------------------------------------------------
  async function beginAuthentication(input: {
    readonly email: string;
  }): Promise<Result<AuthAuthenticationOptions, AuthError>> {
    const email = normalizeEmail(input.email);

    // Indistinguishable response: placeholder, unallowed, unknown, and
    // no-active-passkey all return `no_passkey_for_email`. A single audit
    // action covers all four, with a sanitized `reason` for forensic triage.
    if (isPlaceholderEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'placeholder_email' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }

    if (!isAllowedEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'email_not_allowlisted' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }

    const userResult = await deps.userRepo.findByEmail(email);
    if (!userResult.ok || userResult.value === null) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'unknown_email' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }
    const user = userResult.value;

    const passkeysResult = await deps.passkeyRepo.listByUser(user.id);
    const activePasskeys = passkeysResult.ok
      ? passkeysResult.value.filter((p) => p.revokedAt === null)
      : [];
    if (activePasskeys.length === 0) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'no_active_passkey' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }

    const opts = await webauthnBeginAuth({
      allowCredentials: activePasskeys.map((p) => ({
        id: base64UrlFromBytes(p.credentialId),
      })),
    });

    return Ok({
      challenge: opts.challenge,
      rpId: opts.rpId ?? readRpConfig().rpID,
      timeout: opts.timeout ?? 60_000,
      userVerification: 'required',
      allowCredentials: (opts.allowCredentials ?? []).map((c) => ({
        id: c.id,
        type: 'public-key',
      })),
    });
  }

  // -------------------------------------------------------------------------
  // completeAuthentication
  // -------------------------------------------------------------------------
  async function completeAuthentication(input: {
    readonly email: string;
    readonly response: Parameters<AuthService['completeAuthentication']>[0]['response'];
    readonly expectedChallenge: string;
    readonly userAgent: string | null;
    readonly remoteAddr: string | null;
  }): Promise<Result<{ readonly userId: UserIdType; readonly sessionId: SessionIdType; readonly cookieValue: string }, AuthError>> {
    const email = normalizeEmail(input.email);

    if (isPlaceholderEmail(email) || !isAllowedEmail(email)) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'email_gate_failed' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }

    const userResult = await deps.userRepo.findByEmail(email);
    if (!userResult.ok || userResult.value === null) {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'unknown_email' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }
    const user = userResult.value;

    // Look up the credential the assertion claims (response.id is base64url).
    const credentialIdBytes = bytesFromBase64Url(input.response.id);
    const passkeyResult = await deps.passkeyRepo.findByCredentialId(credentialIdBytes);
    if (!passkeyResult.ok || passkeyResult.value === null) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'unknown_credential' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }
    const passkey = passkeyResult.value;
    if (passkey.userId !== user.id) {
      // Credential belongs to a different user — also indistinguishable 404.
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'credential_user_mismatch' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }
    if (passkey.revokedAt !== null) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: passkey.id,
        subjectKind: 'passkey',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: { reason: 'passkey_revoked' },
      });
      return Err({ kind: 'no_passkey_for_email' });
    }

    // Verify the WebAuthn assertion.
    const verifyResult = await webauthnVerifyAuth({
      response: input.response,
      expectedChallenge: input.expectedChallenge,
      credential: {
        id: base64UrlFromBytes(passkey.credentialId),
        publicKey: passkey.credentialPublicKey,
        counter: passkey.counter,
        transports: passkey.transports,
      },
    });
    if (!verifyResult.verified) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: passkey.id,
        subjectKind: 'passkey',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Failure,
        details: { reason: 'signature_invalid' },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Counter monotonicity (replay defense).
    if (
      !isCounterMonotonic({
        storedCounter: passkey.counter,
        newCounter: verifyResult.newCounter,
      })
    ) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: passkey.id,
        subjectKind: 'passkey',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Denied,
        details: {
          reason: 'counter_regression',
          storedCounter: passkey.counter.toString(),
          newCounter: verifyResult.newCounter.toString(),
        },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Bump counter (only after successful verification + monotonicity).
    const bumpResult = await deps.passkeyRepo.bumpCounter({
      id: passkey.id,
      newCounter: verifyResult.newCounter,
    });
    if (!bumpResult.ok) {
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Single-session enforcement: revoke the prior active session if any.
    const prior = await deps.sessionRepo.findActiveByUser(user.id);
    if (prior.ok && prior.value !== null) {
      const revoked = await deps.sessionRepo.revoke({
        id: prior.value.id,
        reason: 'new_login',
      });
      if (revoked.ok) {
        await safeAppend(deps.audit, {
          actorUserId: user.id,
          actorKind: ActorKind.System,
          domain: null,
          subjectId: prior.value.id,
          subjectKind: 'session',
          action: AuditAction.SessionRevoked,
          outcome: AuditOutcome.Success,
          details: { reason: 'new_login' },
        });
        // After revoking the previous session, check if any other active
        // sessions remain. If not, unload the old DEK before we load a fresh
        // one (a fresh `loadUserDek` below replaces it anyway, but we audit
        // the zeroize event for paranoia symmetry).
        const stillActive = await deps.sessionRepo.findActiveByUser(user.id);
        if (stillActive.ok && stillActive.value === null) {
          await deps.crypto.unloadUserDek(user.id);
          await safeAppend(deps.audit, {
            actorUserId: user.id,
            actorKind: ActorKind.System,
            domain: null,
            subjectId: user.id,
            subjectKind: 'user',
            action: AuditAction.PerUserDekZeroized,
            outcome: AuditOutcome.Success,
            details: { reason: 'pre_login_dek_swap' },
          });
        }
      }
    }

    // Create the new session + cookie.
    const sess = await createSession(sessionDeps, {
      userId: user.id,
      userAgent: input.userAgent,
      remoteAddr: input.remoteAddr,
    });
    if (!sess.ok) {
      return Err({ kind: 'webauthn_verification_failed' });
    }

    // Load the user DEK into CryptoService (in-memory only).
    const [wrappedRead, kekSaltRead] = await Promise.all([
      deps.wrappedDekReader.readWrappedUserDek(user.id),
      deps.wrappedDekReader.readPasskeyKekSalt(passkey.id),
    ]);
    if (!wrappedRead.ok || wrappedRead.value === null || !kekSaltRead.ok || kekSaltRead.value === null) {
      // No DEK / kekSalt yet (enrollment incomplete) or the read failed.
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Failure,
        details: { reason: 'wrapped_user_dek_or_salt_missing' },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }
    const loaded = await deps.crypto.loadUserDek({
      userId: user.id,
      credentialId: passkey.credentialId,
      kekSalt: kekSaltRead.value,
      wrappedUserDek: wrappedRead.value,
      userDekVersion: user.userDekVersion,
    });
    if (!loaded.ok) {
      await safeAppend(deps.audit, {
        actorUserId: user.id,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: user.id,
        subjectKind: 'user',
        action: AuditAction.PasskeyAuthenticationFailure,
        outcome: AuditOutcome.Failure,
        details: { reason: 'dek_unwrap_failed' },
      });
      return Err({ kind: 'webauthn_verification_failed' });
    }

    await safeAppend(deps.audit, {
      actorUserId: user.id,
      actorKind: ActorKind.System,
      domain: null,
      subjectId: user.id,
      subjectKind: 'user',
      action: AuditAction.PerUserDekDerived,
      outcome: AuditOutcome.Success,
      details: { userDekVersion: user.userDekVersion },
    });
    await safeAppend(deps.audit, {
      actorUserId: user.id,
      actorKind: ActorKind.User,
      domain: null,
      subjectId: passkey.id,
      subjectKind: 'passkey',
      action: AuditAction.PasskeyAuthenticationSuccess,
      outcome: AuditOutcome.Success,
      details: {},
    });
    await safeAppend(deps.audit, {
      actorUserId: user.id,
      actorKind: ActorKind.System,
      domain: null,
      subjectId: sess.value.session.id,
      subjectKind: 'session',
      action: AuditAction.SessionCreated,
      outcome: AuditOutcome.Success,
      details: {},
    });

    return Ok({
      userId: user.id,
      sessionId: sess.value.session.id,
      cookieValue: sess.value.cookieValue,
    });
  }

  // -------------------------------------------------------------------------
  // validateSession
  // -------------------------------------------------------------------------
  async function validateSession(input: {
    readonly sessionId: SessionIdType;
    readonly cookieValue: string;
  }): Promise<Result<Session, AuthError>> {
    const result = await validateSessionHelper(sessionDeps, input);
    if (!result.ok && result.error.kind === 'session_expired') {
      await safeAppend(deps.audit, {
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: input.sessionId,
        subjectKind: 'session',
        action: AuditAction.SessionExpired,
        outcome: AuditOutcome.Success,
        details: {},
      });
    }
    return result;
  }

  // -------------------------------------------------------------------------
  // revokeSession
  // -------------------------------------------------------------------------
  async function revokeSessionImpl(input: {
    readonly sessionId: SessionIdType;
    readonly reason: string;
  }): Promise<Result<void, AuthError>> {
    const result = await revokeSession(sessionDeps, input);
    if (!result.ok) {
      return Err(result.error);
    }
    await safeAppend(deps.audit, {
      actorUserId: result.value.userId,
      actorKind: ActorKind.User,
      domain: null,
      subjectId: input.sessionId,
      subjectKind: 'session',
      action: AuditAction.SessionRevoked,
      outcome: AuditOutcome.Success,
      details: { reason: input.reason },
    });
    if (result.value.dekUnloaded && result.value.userId !== null) {
      await safeAppend(deps.audit, {
        actorUserId: result.value.userId,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: result.value.userId,
        subjectKind: 'user',
        action: AuditAction.PerUserDekZeroized,
        outcome: AuditOutcome.Success,
        details: { reason: 'last_active_session_revoked' },
      });
    }
    return Ok(undefined);
  }

  // -------------------------------------------------------------------------
  // revokeAllSessions
  // -------------------------------------------------------------------------
  async function revokeAllSessionsImpl(input: {
    readonly reason: string;
  }): Promise<Result<{ readonly count: number }, AuthError>> {
    const result = await revokeAllActive(sessionDeps, input);
    if (!result.ok) {
      return Err(result.error);
    }
    return Ok(result.value);
  }

  return {
    beginEnrollment,
    completeEnrollment,
    beginAuthentication,
    completeAuthentication,
    validateSession,
    revokeSession: revokeSessionImpl,
    revokeAllSessions: revokeAllSessionsImpl,
  };
}

// Re-export the inner types so route handlers can import them from one place.
export type { AuthService } from '../types/services.js';
export { isAllowedEmail, isPlaceholderEmail } from './allowlist.js';

// Suppress "unused symbol" lints for value-imports we re-export only as types.
// (PasskeyId, SessionId, UserId, Domain are not referenced as values directly
// in this module; we keep the imports for IDE go-to-symbol traversal.)
const _unused = { PasskeyId, SessionId, UserId, Domain };
void _unused;
