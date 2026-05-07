// Greylock — POST /api/auth/logout
// =============================================================================
// AGENT-AUTH (Phase 2). Revokes the caller's session, unloads the user DEK if
// no other active session remains, clears the session cookie.
//
// Auth: session.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { LogoutResponseSchema } from '../../../../lib/types/zod-schemas.js';
import { SessionId } from '../../../../lib/types/domain.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
import { getAuthService } from '../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  const sessionConfig = readSessionConfig();

  const cookie = req.cookies.get(sessionConfig.cookieName);
  if (cookie === undefined) {
    return err(401, 'no_session', 'no active session');
  }
  const unsealed = await unsealSessionCookie({
    cookieValue: cookie.value,
    config: sessionConfig,
  });
  if (!unsealed.ok) {
    // Even though there's no usable session, clear the cookie defensively.
    const res = err(401, 'invalid_session', 'session invalid');
    res.cookies.set({
      name: sessionConfig.cookieName,
      value: '',
      sameSite: 'strict',
      secure: true,
      httpOnly: true,
      path: '/',
      maxAge: 0,
    });
    return res;
  }

  const auth = await getAuthService();
  const result = await auth.revokeSession({
    sessionId: SessionId(unsealed.value.sessionId),
    reason: 'logout',
  });
  // We don't surface a partial failure to the user — clearing the cookie is
  // always safe. Audit emit happens inside AuthService.
  void result;

  const responseBody = LogoutResponseSchema.parse({ ok: true });
  const res = NextResponse.json(responseBody, { status: 200 });
  res.cookies.set({
    name: sessionConfig.cookieName,
    value: '',
    sameSite: 'strict',
    secure: true,
    httpOnly: true,
    path: '/',
    maxAge: 0,
  });
  return res;
}
