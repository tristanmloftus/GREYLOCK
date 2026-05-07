// Greylock — POST /api/auth/registration/begin
// =============================================================================
// AGENT-AUTH (Phase 2). Generates a WebAuthn registration challenge and
// stashes it in an ephemeral ceremony cookie. Validates the enrollment-token
// header against an EnrollmentTokenRepository (stub interface defined in
// `lib/auth/enrollment-token.ts`; AGENT-DB will implement in Phase 3 — until
// then the dynamic import in services-registry surfaces 503).
//
// Auth: enrollmentToken (header `x-enrollment-token`).
// Rate limit: 5 attempts / 15 min / (IP, email).
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { sealData } from 'iron-session';

import {
  RegistrationBeginRequestSchema,
  RegistrationBeginResponseSchema,
} from '../../../../../lib/types/zod-schemas.js';
import { readRpConfig } from '../../../../../lib/auth/webauthn.js';
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

const CEREMONY_COOKIE_NAME = 'greylock_reg_ceremony';
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
  // Body validation.
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = RegistrationBeginRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // Enrollment-token header.
  const tokenHeader = req.headers.get('x-enrollment-token');
  if (tokenHeader === null || tokenHeader.length === 0) {
    return err(401, 'enrollment_token_missing', 'enrollment token required');
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
    flow: 'auth:enroll',
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

  // Validate enrollment token via dynamic dependency. AGENT-DB will export
  // `enrollmentTokenRepo` from `lib/db/index.ts` once the EnrollmentToken row
  // is added. Until then this dynamic import surfaces 503.
  interface EnrollmentRepoShape {
    verify(input: { token: string }): Promise<
      { ok: true; value: { tokenId: string; email: string } } | { ok: false; error: { kind: string } }
    >;
    consume(input: { tokenId: string }): Promise<
      { ok: true } | { ok: false; error: { kind: string } }
    >;
  }
  let enrollmentRepo: EnrollmentRepoShape;
  try {
    const path = '../../../../../lib/db/index.js';
    const mod = (await import(/* @vite-ignore */ path)) as {
      readonly enrollmentTokenRepo?: EnrollmentRepoShape;
    };
    if (mod.enrollmentTokenRepo === undefined) {
      return err(503, 'service_unavailable', 'enrollment token store unavailable');
    }
    enrollmentRepo = mod.enrollmentTokenRepo;
  } catch {
    return err(503, 'service_unavailable', 'enrollment token store unavailable');
  }
  const verified = await enrollmentRepo.verify({ token: tokenHeader });
  if (!verified.ok) {
    return err(401, 'enrollment_token_invalid', 'enrollment token invalid or expired');
  }
  if (verified.value.email.trim().toLowerCase() !== parsed.data.email) {
    return err(401, 'enrollment_token_invalid', 'enrollment token invalid for this email');
  }

  // Generate options.
  const auth = await getAuthService();
  const result = await auth.beginEnrollment({
    email: parsed.data.email,
    displayName: parsed.data.displayName,
    role: parsed.data.role ?? 'member',
  });
  if (!result.ok) {
    if (result.error.kind === 'placeholder_email_rejected') {
      return err(400, 'placeholder_email_rejected', 'placeholder email cannot enroll');
    }
    if (result.error.kind === 'email_not_allowlisted') {
      return err(400, 'email_not_allowlisted', 'email not in allowlist');
    }
    if (result.error.kind === 'passkey_already_enrolled') {
      return err(409, 'passkey_already_enrolled', 'a passkey is already enrolled for this email');
    }
    return err(400, 'registration_begin_failed', 'failed to begin registration');
  }

  // Stash the challenge in an ephemeral ceremony cookie.
  const sessionConfig = readSessionConfig();
  const ceremonyValue = await sealData(
    {
      flow: 'registration',
      email: parsed.data.email,
      challenge: result.value.challenge,
      createdAtMs: Date.now(),
    },
    { password: sessionConfig.secret, ttl: CEREMONY_TTL_SECONDS },
  );

  const rp = readRpConfig();
  const responseBody = RegistrationBeginResponseSchema.parse({
    challenge: result.value.challenge,
    rp: { id: rp.rpID, name: rp.rpName },
    user: {
      id: result.value.user.id,
      name: result.value.user.name,
      displayName: result.value.user.displayName,
    },
    pubKeyCredParams: result.value.pubKeyCredParams.map((p) => ({ type: 'public-key', alg: p.alg })),
    timeout: result.value.timeout,
    attestation: 'none',
    authenticatorSelection: {
      residentKey: 'required',
      userVerification: 'required',
    },
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
