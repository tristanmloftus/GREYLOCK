// Greylock — POST /api/plaid/exchange
// =============================================================================
// AGENT-PLAID (Phase 3). Exchanges a Plaid public_token for an access_token
// and persists the encrypted Item row. Auth: session (`pccMember` if
// domain==='pcc'). The plaintext access_token NEVER leaves PlaidService —
// route returns only `{ itemId }`.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  PlaidExchangeRequestSchema,
  PlaidExchangeResponseSchema,
} from '../../../../lib/types/zod-schemas.js';
import type { PlaidPublicToken as PlaidPublicTokenT } from '../../../../lib/types/domain.js';
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

const asPlaidPublicToken = (s: string): PlaidPublicTokenT => s as PlaidPublicTokenT;

export async function POST(req: NextRequest): Promise<NextResponse> {
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = PlaidExchangeRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // Session.
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

  // PCC gate.
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

  // Exchange.
  let plaid;
  try {
    plaid = await getPlaidService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const result = await plaid.exchangePublicToken({
    userId,
    domain: parsed.data.domain,
    publicToken: asPlaidPublicToken(parsed.data.publicToken),
    institutionId: parsed.data.institutionId,
    institutionName: parsed.data.institutionName,
  });
  if (!result.ok) {
    if (result.error.kind === 'invalid_public_token') {
      return err(400, 'invalid_public_token', 'invalid public token');
    }
    return err(502, 'plaid_api_error', 'plaid call failed');
  }

  const body = PlaidExchangeResponseSchema.parse({ itemId: result.value.itemId });
  return NextResponse.json(body, { status: 200 });
}
