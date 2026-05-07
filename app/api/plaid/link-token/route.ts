// Greylock — POST /api/plaid/link-token
// =============================================================================
// AGENT-PLAID (Phase 3). Mints a Plaid link_token. Auth: session
// (`pccMember` additionally required when domain==='pcc').
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  PlaidLinkTokenRequestSchema,
  PlaidLinkTokenResponseSchema,
} from '../../../../lib/types/zod-schemas.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
import { SessionId } from '../../../../lib/types/domain.js';
import {
  getAuthService,
  getFullRepos,
  getPlaidService,
} from '../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  // 1. Parse body.
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = PlaidLinkTokenRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // 2. Validate session.
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
    return err(401, 'invalid_session', 'session invalid');
  }
  let auth;
  try {
    auth = await getAuthService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const sessionRes = await auth.validateSession({
    sessionId: SessionId(unsealed.value.sessionId),
    cookieValue: cookie.value,
  });
  if (!sessionRes.ok) {
    return err(401, 'invalid_session', 'session invalid');
  }
  const userId = sessionRes.value.userId;

  // 3. PCC route gating.
  if (parsed.data.domain === 'pcc') {
    let repos;
    try {
      repos = await getFullRepos();
    } catch {
      return err(503, 'service_unavailable', 'service temporarily unavailable');
    }
    const memberRes = await repos.pccMembershipRepo.isActiveMember(userId);
    if (!memberRes.ok || !memberRes.value) {
      return err(403, 'unauthorized', 'pcc membership required');
    }
  }

  // 4. Mint link token.
  let plaid;
  try {
    plaid = await getPlaidService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const result = await plaid.mintLinkToken({
    userId,
    domain: parsed.data.domain,
    products: parsed.data.products,
  });
  if (!result.ok) {
    if (result.error.kind === 'plaid_api_error') {
      return err(502, 'plaid_api_error', 'plaid call failed');
    }
    return err(502, 'plaid_api_error', 'plaid call failed');
  }

  // 5. Response.
  const body = PlaidLinkTokenResponseSchema.parse({
    linkToken: result.value.linkToken,
    expiresAt: result.value.expiresAt.toISOString(),
  });
  return NextResponse.json(body, { status: 200 });
}
