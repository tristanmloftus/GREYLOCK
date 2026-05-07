// Greylock — iron-session config + session lifecycle helpers
// =============================================================================
// AGENT-AUTH (Phase 2). Centralizes:
//   - the iron-session cookie configuration (encrypted/signed cookie body)
//   - session creation, validation, sliding window, revocation
//   - idle/absolute timeout enforcement
//
// Cookie attributes (locked):
//   SameSite=Strict; Secure; HttpOnly; Path=/
//
// Two cookies are issued:
//   - SESSION_COOKIE_NAME (long-lived; references a server-side `Session` row)
//   - <ceremony-cookie>   (ephemeral; carries a registration/auth challenge)
//
// The cookie body holds an encrypted `{ sessionId, csrfNonce }` object — the
// random `Session.id` is the ground truth (server-side). iron-session encrypts
// + signs the body with `SESSION_SECRET`.
//
// IMPORTANT: this module never logs the cookie value, the Session.id, or any
// challenge bytes. Diagnostics surface as opaque error kinds only.
// =============================================================================

import { sealData, unsealData } from 'iron-session';

import { Err, Ok } from '../types/domain.js';
import type {
  AuthError,
  Result,
  Session,
  SessionId,
  UserId,
} from '../types/domain.js';
import type { CryptoService, SessionRepository } from '../types/services.js';

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

export interface SessionConfig {
  readonly cookieName: string;
  readonly secret: string;
  readonly idleMinutes: number;
  readonly absoluteHours: number;
}

export function readSessionConfig(): SessionConfig {
  const cookieName = process.env['SESSION_COOKIE_NAME'];
  const secret = process.env['SESSION_SECRET'];
  const idleStr = process.env['SESSION_IDLE_MINUTES'] ?? '30';
  const absoluteStr = process.env['SESSION_ABSOLUTE_HOURS'] ?? '8';

  if (cookieName === undefined || cookieName.length === 0) {
    throw new Error('SESSION_COOKIE_NAME is not set');
  }
  if (secret === undefined || secret.length < 32) {
    throw new Error('SESSION_SECRET is not set or shorter than 32 characters');
  }
  const idleMinutes = Number.parseInt(idleStr, 10);
  const absoluteHours = Number.parseInt(absoluteStr, 10);
  if (!Number.isFinite(idleMinutes) || idleMinutes <= 0) {
    throw new Error('SESSION_IDLE_MINUTES must be a positive integer');
  }
  if (!Number.isFinite(absoluteHours) || absoluteHours <= 0) {
    throw new Error('SESSION_ABSOLUTE_HOURS must be a positive integer');
  }
  return { cookieName, secret, idleMinutes, absoluteHours };
}

/** Set-Cookie attributes locked by spec. */
export interface CookieAttributes {
  readonly sameSite: 'strict';
  readonly secure: true;
  readonly httpOnly: true;
  readonly path: '/';
  /** Cookie max-age in seconds (matches absolute timeout). */
  readonly maxAge: number;
}

export function buildCookieAttributes(config: SessionConfig): CookieAttributes {
  return {
    sameSite: 'strict',
    secure: true,
    httpOnly: true,
    path: '/',
    maxAge: config.absoluteHours * 60 * 60,
  };
}

/** Serialize cookie attributes for a `Set-Cookie` header. The cookie value is
 *  produced separately via iron-session; this function only renders the
 *  attribute suffix. Caller is responsible for `name=value`. */
export function renderCookieHeader(args: {
  readonly name: string;
  readonly value: string;
  readonly attributes: CookieAttributes;
}): string {
  const a = args.attributes;
  return [
    `${args.name}=${args.value}`,
    `Path=${a.path}`,
    `Max-Age=${String(a.maxAge)}`,
    'HttpOnly',
    'Secure',
    `SameSite=${a.sameSite === 'strict' ? 'Strict' : a.sameSite}`,
  ].join('; ');
}

// -----------------------------------------------------------------------------
// Cookie body shape
// -----------------------------------------------------------------------------

export interface SessionCookieBody {
  readonly sessionId: SessionId;
  /** Per-cookie nonce that lets us correlate `validateSession` with the cookie
   *  body during e2e tests. Not used for security itself — iron-session's
   *  signature is the trust gate. */
  readonly nonce: string;
}

export async function sealSessionCookie(args: {
  readonly body: SessionCookieBody;
  readonly config: SessionConfig;
  /** Cookie ttl in seconds; defaults to absolute window. */
  readonly ttlSeconds?: number;
}): Promise<string> {
  const ttl = args.ttlSeconds ?? args.config.absoluteHours * 60 * 60;
  return sealData({ ...args.body }, { password: args.config.secret, ttl });
}

export async function unsealSessionCookie(args: {
  readonly cookieValue: string;
  readonly config: SessionConfig;
}): Promise<Result<SessionCookieBody, AuthError>> {
  try {
    const data = await unsealData<SessionCookieBody>(args.cookieValue, {
      password: args.config.secret,
    });
    if (
      typeof data.sessionId !== 'string' ||
      data.sessionId.length === 0 ||
      typeof data.nonce !== 'string' ||
      data.nonce.length === 0
    ) {
      return Err({ kind: 'session_not_found' });
    }
    return Ok({ sessionId: data.sessionId, nonce: data.nonce });
  } catch {
    // unseal failure: treat as session-invalid. Never surface the underlying
    // error string — it could leak signature timing.
    return Err({ kind: 'session_not_found' });
  }
}

// -----------------------------------------------------------------------------
// Lifecycle dependencies
// -----------------------------------------------------------------------------

export interface SessionDeps {
  readonly sessionRepo: SessionRepository;
  readonly crypto: CryptoService;
  readonly config: SessionConfig;
  readonly now: () => Date;
  /** Generate a per-cookie nonce. Deterministic in tests. */
  readonly randomNonce: () => string;
}

// -----------------------------------------------------------------------------
// Create
// -----------------------------------------------------------------------------

export interface CreateSessionInput {
  readonly userId: UserId;
  readonly userAgent: string | null;
  readonly remoteAddr: string | null;
}

export interface CreateSessionResult {
  readonly session: Session;
  readonly cookieValue: string;
  readonly cookieAttributes: CookieAttributes;
}

export async function createSession(
  deps: SessionDeps,
  input: CreateSessionInput,
): Promise<Result<CreateSessionResult, AuthError>> {
  const now = deps.now();
  const idleTimeoutAt = new Date(now.getTime() + deps.config.idleMinutes * 60 * 1000);
  const expiresAt = new Date(now.getTime() + deps.config.absoluteHours * 60 * 60 * 1000);

  const created = await deps.sessionRepo.create({
    userId: input.userId,
    expiresAt,
    idleTimeoutAt,
    userAgent: input.userAgent,
    remoteAddr: input.remoteAddr,
  });
  if (!created.ok) {
    return Err({ kind: 'session_not_found' });
  }

  const cookieValue = await sealSessionCookie({
    body: { sessionId: created.value.id, nonce: deps.randomNonce() },
    config: deps.config,
  });

  return Ok({
    session: created.value,
    cookieValue,
    cookieAttributes: buildCookieAttributes(deps.config),
  });
}

// -----------------------------------------------------------------------------
// Validate (with sliding window)
// -----------------------------------------------------------------------------

export interface ValidateSessionInput {
  readonly sessionId: SessionId;
  readonly cookieValue: string;
}

/** Validate a session id + cookie body, enforce idle + absolute timeouts, and
 *  slide `idleTimeoutAt`. On expiry, revoke the session and unload the user
 *  DEK if no other active sessions remain. */
export async function validateSession(
  deps: SessionDeps,
  input: ValidateSessionInput,
): Promise<Result<Session, AuthError>> {
  const cookie = await unsealSessionCookie({
    cookieValue: input.cookieValue,
    config: deps.config,
  });
  if (!cookie.ok) {
    return cookie;
  }
  if (cookie.value.sessionId !== input.sessionId) {
    return Err({ kind: 'session_not_found' });
  }

  const found = await deps.sessionRepo.findActiveById(input.sessionId);
  if (!found.ok) {
    return Err({ kind: 'session_not_found' });
  }
  if (found.value === null) {
    return Err({ kind: 'session_not_found' });
  }
  const session = found.value;
  return enforceIdleAndAbsoluteTimeouts(deps, session);
}

/** Helper exposed for tests: pure timeout-and-slide logic given a `Session`. */
export async function enforceIdleAndAbsoluteTimeouts(
  deps: SessionDeps,
  session: Session,
): Promise<Result<Session, AuthError>> {
  const now = deps.now();

  if (session.expiresAt.getTime() <= now.getTime()) {
    await expireAndUnload(deps, session, 'absolute_timeout');
    return Err({ kind: 'session_expired' });
  }
  if (session.idleTimeoutAt.getTime() <= now.getTime()) {
    await expireAndUnload(deps, session, 'idle_timeout');
    return Err({ kind: 'session_expired' });
  }

  const newIdleTimeoutAt = new Date(now.getTime() + deps.config.idleMinutes * 60 * 1000);
  const touched = await deps.sessionRepo.touch({
    id: session.id,
    newIdleTimeoutAt,
  });
  if (!touched.ok) {
    return Err({ kind: 'session_not_found' });
  }

  return Ok({ ...session, idleTimeoutAt: newIdleTimeoutAt, lastActivityAt: now });
}

async function expireAndUnload(
  deps: SessionDeps,
  session: Session,
  _reason: string,
): Promise<void> {
  const revoked = await deps.sessionRepo.revoke({ id: session.id, reason: 'expired' });
  if (!revoked.ok) {
    return;
  }
  // Unload DEK only if no other active sessions remain for this user.
  const active = await deps.sessionRepo.findActiveByUser(session.userId);
  if (active.ok && active.value === null) {
    await deps.crypto.unloadUserDek(session.userId);
  }
}

// -----------------------------------------------------------------------------
// Revoke
// -----------------------------------------------------------------------------

export async function revokeSession(
  deps: SessionDeps,
  args: { readonly sessionId: SessionId; readonly reason: string },
): Promise<Result<{ readonly userId: UserId | null; readonly dekUnloaded: boolean }, AuthError>> {
  const found = await deps.sessionRepo.findActiveById(args.sessionId);
  if (!found.ok) {
    return Err({ kind: 'session_not_found' });
  }
  const session = found.value;
  if (session === null) {
    return Ok({ userId: null, dekUnloaded: false });
  }
  const revoked = await deps.sessionRepo.revoke({ id: session.id, reason: args.reason });
  if (!revoked.ok) {
    return Err({ kind: 'session_not_found' });
  }
  const active = await deps.sessionRepo.findActiveByUser(session.userId);
  let dekUnloaded = false;
  if (active.ok && active.value === null) {
    await deps.crypto.unloadUserDek(session.userId);
    dekUnloaded = true;
  }
  return Ok({ userId: session.userId, dekUnloaded });
}

export async function revokeAllActive(
  deps: SessionDeps,
  args: { readonly reason: string },
): Promise<Result<{ readonly count: number }, AuthError>> {
  const result = await deps.sessionRepo.revokeAllActive({ reason: args.reason });
  if (!result.ok) {
    return Err({ kind: 'session_not_found' });
  }
  return Ok({ count: result.value.count });
}
