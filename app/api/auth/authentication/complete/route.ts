// Greylock — POST /api/auth/authentication/complete
// =============================================================================
// AGENT-AUTH (Phase 2). Verifies the WebAuthn assertion against the challenge
// stashed in the ceremony cookie, enforces single-session-per-user, creates a
// new session, sets the iron-session cookie. Counter monotonicity is enforced
// inside `AuthService.completeAuthentication`.
//
// Auth: none (the assertion is the auth). Rate-limited per (IP, email).
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { unsealData } from 'iron-session';

import {
  AuthenticationCompleteRequestSchema,
  AuthenticationCompleteResponseSchema,
} from '../../../../../lib/types/zod-schemas.js';
import {
  buildCookieAttributes,
  readSessionConfig,
} from '../../../../../lib/auth/session.js';
import {
  consume,
  rateLimitBucketKey,
  readRateLimitConfig,
} from '../../../../../lib/auth/rate-limit.js';
import {
  getAuthService,
  getRepos,
} from '../../../../../lib/runtime/services-registry.js';

const CEREMONY_COOKIE_NAME = 'greylock_auth_ceremony';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string; readonly retryAfterSeconds?: number };
}

function err(status: number, code: string, message: string, extra?: { retryAfterSeconds: number }): NextResponse {
  const body: ErrorBody = {
    error: extra !== undefined ? { code, message, retryAfterSeconds: extra.retryAfterSeconds } : { code, message },
  };
  return NextResponse.json(body, { status });
}

function readRemoteAddr(req: NextRequest): string | null {
  return req.headers.get('x-forwarded-for') ?? null;
}

interface CeremonyBody {
  readonly flow: string;
  readonly email: string;
  readonly challenge: string;
  readonly createdAtMs: number;
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = AuthenticationCompleteRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // Rate limit.
  let repos;
  try {
    repos = await getRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const rlConfig = readRateLimitConfig();
  const bucketKey = rateLimitBucketKey({
    flow: 'auth:assert',
    remoteAddr: readRemoteAddr(req),
    email: parsed.data.email,
  });
  const rlResult = await consume({
    repo: repos.rateLimitRepo,
    bucketKey,
    now: new Date(),
    config: rlConfig,
  });
  if (!rlResult.ok) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  if (rlResult.value.kind === 'tripped') {
    return err(429, 'rate_limited', 'too many requests', {
      retryAfterSeconds: rlResult.value.retryAfterSeconds,
    });
  }

  // Read ceremony cookie.
  const ceremonyCookie = req.cookies.get(CEREMONY_COOKIE_NAME);
  if (ceremonyCookie === undefined) {
    return err(400, 'ceremony_cookie_missing', 'authentication ceremony cookie missing');
  }
  const sessionConfig = readSessionConfig();
  let ceremony: CeremonyBody;
  try {
    ceremony = await unsealData<CeremonyBody>(ceremonyCookie.value, {
      password: sessionConfig.secret,
    });
  } catch {
    return err(400, 'ceremony_cookie_invalid', 'authentication ceremony cookie invalid');
  }
  if (ceremony.flow !== 'authentication') {
    return err(400, 'ceremony_cookie_invalid', 'wrong ceremony flow');
  }
  if (ceremony.email !== parsed.data.email) {
    return err(400, 'ceremony_cookie_invalid', 'email mismatch with ceremony');
  }

  const auth = await getAuthService();
  // Strip undefined optional fields for `exactOptionalPropertyTypes`.
  const responseField: Parameters<typeof auth.completeAuthentication>[0]['response'] = {
    id: parsed.data.response.id,
    rawId: parsed.data.response.rawId,
    response: {
      authenticatorData: parsed.data.response.response.authenticatorData,
      clientDataJSON: parsed.data.response.response.clientDataJSON,
      signature: parsed.data.response.response.signature,
      ...(parsed.data.response.response.userHandle !== undefined
        ? { userHandle: parsed.data.response.response.userHandle }
        : {}),
    },
    clientExtensionResults: parsed.data.response.clientExtensionResults,
    type: parsed.data.response.type,
    ...(parsed.data.response.authenticatorAttachment !== undefined
      ? { authenticatorAttachment: parsed.data.response.authenticatorAttachment }
      : {}),
  };
  const result = await auth.completeAuthentication({
    email: parsed.data.email,
    response: responseField,
    expectedChallenge: ceremony.challenge,
    userAgent: req.headers.get('user-agent'),
    remoteAddr: readRemoteAddr(req),
  });
  if (!result.ok) {
    if (result.error.kind === 'no_passkey_for_email') {
      return err(404, 'no_passkey_for_email', 'no passkey found');
    }
    if (result.error.kind === 'webauthn_verification_failed') {
      return err(400, 'webauthn_verification_failed', 'assertion could not be verified');
    }
    if (result.error.kind === 'rate_limited') {
      return err(429, 'rate_limited', 'too many requests', {
        retryAfterSeconds: result.error.retryAfterSeconds,
      });
    }
    return err(400, 'authentication_complete_failed', 'failed to complete authentication');
  }

  const responseBody = AuthenticationCompleteResponseSchema.parse({
    userId: result.value.userId,
    sessionId: result.value.sessionId,
  });

  const res = NextResponse.json(responseBody, { status: 200 });
  // Issue session cookie via Next response cookie API. Locked attributes from
  // the session helper.
  const attrs = buildCookieAttributes(sessionConfig);
  res.cookies.set({
    name: sessionConfig.cookieName,
    value: result.value.cookieValue,
    sameSite: attrs.sameSite,
    secure: attrs.secure,
    httpOnly: attrs.httpOnly,
    path: attrs.path,
    maxAge: attrs.maxAge,
  });
  // Clear the ephemeral ceremony cookie.
  res.cookies.set({
    name: CEREMONY_COOKIE_NAME,
    value: '',
    sameSite: 'strict',
    secure: true,
    httpOnly: true,
    path: '/',
    maxAge: 0,
  });
  return res;
}
