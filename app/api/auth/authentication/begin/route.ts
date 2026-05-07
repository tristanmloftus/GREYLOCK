// Greylock — POST /api/auth/authentication/begin
// =============================================================================
// AGENT-AUTH (Phase 2). Generates a WebAuthn authentication challenge for a
// known email. Indistinguishable 404 for unknown / unallowed / placeholder /
// no-active-passkey cases (THREAT_MODEL §1.5 / API §3).
//
// Auth: none. Rate-limited per (IP, email).
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { sealData } from 'iron-session';

import {
  AuthenticationBeginRequestSchema,
  AuthenticationBeginResponseSchema,
} from '../../../../../lib/types/zod-schemas.js';
import {
  consume,
  rateLimitBucketKey,
  readRateLimitConfig,
} from '../../../../../lib/auth/rate-limit.js';
import { readSessionConfig } from '../../../../../lib/auth/session.js';
import {
  getAuthService,
  getRepos,
} from '../../../../../lib/runtime/services-registry.js';

const CEREMONY_COOKIE_NAME = 'greylock_auth_ceremony';
const CEREMONY_TTL_SECONDS = 5 * 60;

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

export async function POST(req: NextRequest): Promise<NextResponse> {
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = AuthenticationBeginRequestSchema.safeParse(json);
  if (!parsed.success) {
    // Even validation failure shouldn't leak email-shape vs allowlist-shape;
    // 400 here indicates the email field couldn't be parsed at all.
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // Rate limit (per IP+email).
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

  const auth = await getAuthService();
  const result = await auth.beginAuthentication({ email: parsed.data.email });
  if (!result.ok) {
    if (result.error.kind === 'no_passkey_for_email') {
      return err(404, 'no_passkey_for_email', 'no passkey found');
    }
    if (result.error.kind === 'rate_limited') {
      return err(429, 'rate_limited', 'too many requests', {
        retryAfterSeconds: result.error.retryAfterSeconds,
      });
    }
    return err(400, 'authentication_begin_failed', 'failed to begin authentication');
  }

  // Stash challenge in ceremony cookie.
  const sessionConfig = readSessionConfig();
  const ceremonyValue = await sealData(
    {
      flow: 'authentication',
      email: parsed.data.email,
      challenge: result.value.challenge,
      createdAtMs: Date.now(),
    },
    { password: sessionConfig.secret, ttl: CEREMONY_TTL_SECONDS },
  );

  const responseBody = AuthenticationBeginResponseSchema.parse({
    challenge: result.value.challenge,
    rpId: result.value.rpId,
    timeout: result.value.timeout,
    userVerification: 'required',
    allowCredentials: result.value.allowCredentials.map((c) => ({ id: c.id, type: 'public-key' })),
  });

  const res = NextResponse.json(responseBody, { status: 200 });
  res.cookies.set({
    name: CEREMONY_COOKIE_NAME,
    value: ceremonyValue,
    sameSite: 'strict',
    secure: true,
    httpOnly: true,
    path: '/',
    maxAge: CEREMONY_TTL_SECONDS,
  });
  return res;
}
