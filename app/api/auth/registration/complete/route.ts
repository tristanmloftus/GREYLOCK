// Greylock — POST /api/auth/registration/complete
// =============================================================================
// AGENT-AUTH (Phase 2). Verifies the WebAuthn attestation against the challenge
// stashed in the ceremony cookie, persists User + Passkey, derives + wraps the
// per-user DEK, and consumes the enrollment-token row so it cannot replay.
//
// Auth: enrollmentToken header + ceremony cookie.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { unsealData } from 'iron-session';

import {
  RegistrationCompleteRequestSchema,
  RegistrationCompleteResponseSchema,
} from '../../../../../lib/types/zod-schemas.js';
import { readSessionConfig } from '../../../../../lib/auth/session.js';
import { getAuthService } from '../../../../../lib/runtime/services-registry.js';

const CEREMONY_COOKIE_NAME = 'greylock_reg_ceremony';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
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
  const parsed = RegistrationCompleteRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  const tokenHeader = req.headers.get('x-enrollment-token');
  if (tokenHeader === null || tokenHeader.length === 0) {
    return err(401, 'enrollment_token_missing', 'enrollment token required');
  }

  // Read ceremony cookie.
  const ceremonyCookie = req.cookies.get(CEREMONY_COOKIE_NAME);
  if (ceremonyCookie === undefined) {
    return err(400, 'ceremony_cookie_missing', 'registration ceremony cookie missing');
  }
  const sessionConfig = readSessionConfig();
  let ceremony: CeremonyBody;
  try {
    ceremony = await unsealData<CeremonyBody>(ceremonyCookie.value, {
      password: sessionConfig.secret,
    });
  } catch {
    return err(400, 'ceremony_cookie_invalid', 'registration ceremony cookie invalid');
  }
  if (ceremony.flow !== 'registration') {
    return err(400, 'ceremony_cookie_invalid', 'wrong ceremony flow');
  }
  if (ceremony.email !== parsed.data.email) {
    return err(400, 'ceremony_cookie_invalid', 'email mismatch with ceremony');
  }

  // Validate enrollment token (verify; do NOT consume yet — only consume on
  // a successful completeEnrollment so a bad attestation can be retried).
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

  // Complete registration via AuthService. We strip undefined optional fields
  // explicitly so `exactOptionalPropertyTypes` is satisfied.
  const auth = await getAuthService();
  const responseField: Parameters<typeof auth.completeEnrollment>[0]['response'] = {
    id: parsed.data.response.id,
    rawId: parsed.data.response.rawId,
    response: {
      attestationObject: parsed.data.response.response.attestationObject,
      clientDataJSON: parsed.data.response.response.clientDataJSON,
      ...(parsed.data.response.response.transports !== undefined
        ? { transports: parsed.data.response.response.transports }
        : {}),
    },
    clientExtensionResults: parsed.data.response.clientExtensionResults,
    type: parsed.data.response.type,
    ...(parsed.data.response.authenticatorAttachment !== undefined
      ? { authenticatorAttachment: parsed.data.response.authenticatorAttachment }
      : {}),
  };
  const result = await auth.completeEnrollment({
    email: parsed.data.email,
    response: responseField,
    expectedChallenge: ceremony.challenge,
    deviceLabel: parsed.data.deviceLabel,
  });
  if (!result.ok) {
    if (result.error.kind === 'placeholder_email_rejected') {
      return err(400, 'placeholder_email_rejected', 'placeholder email cannot enroll');
    }
    if (result.error.kind === 'email_not_allowlisted') {
      return err(400, 'email_not_allowlisted', 'email not in allowlist');
    }
    if (result.error.kind === 'webauthn_verification_failed') {
      return err(400, 'webauthn_verification_failed', 'attestation could not be verified');
    }
    if (result.error.kind === 'passkey_already_enrolled') {
      return err(409, 'passkey_already_enrolled', 'a passkey is already enrolled for this email');
    }
    return err(400, 'registration_complete_failed', 'failed to complete registration');
  }

  // Consume the enrollment token now that registration succeeded.
  await enrollmentRepo.consume({ tokenId: verified.value.tokenId });

  const responseBody = RegistrationCompleteResponseSchema.parse({
    userId: result.value.userId,
    passkeyId: result.value.passkeyId,
  });

  const res = NextResponse.json(responseBody, { status: 200 });
  // Clear the ceremony cookie.
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
