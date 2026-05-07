// Greylock — POST /api/plaid/items/remove
// =============================================================================
// AGENT-PLAID (Phase 3). Soft-removes an Item: calls Plaid `item/remove`
// upstream, then `softRemove` in the DB. PCC items require membership.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  PlaidItemRemoveRequestSchema,
  PlaidItemRemoveResponseSchema,
} from '../../../../../lib/types/zod-schemas.js';
import { ItemId, SessionId } from '../../../../../lib/types/domain.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../../lib/auth/session.js';
import {
  getAuthService,
  getFullRepos,
  getPlaidService,
} from '../../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = PlaidItemRemoveRequestSchema.safeParse(json);
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

  // Look up the item so we can scope-gate visibility BEFORE calling Plaid.
  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const itemId = ItemId(parsed.data.itemId);
  // Try personal scope first; if that returns null, try pcc (membership-gated).
  const personalRes = await repos.itemRepo.findById(
    { kind: 'personal', userId },
    itemId,
  );
  let visible = false;
  if (personalRes.ok && personalRes.value !== null) {
    visible = true;
  } else {
    const memberRes = await repos.pccMembershipRepo.isActiveMember(userId);
    if (memberRes.ok && memberRes.value) {
      const pccRes = await repos.itemRepo.findById(
        { kind: 'pcc', memberOfUserId: userId },
        itemId,
      );
      if (pccRes.ok && pccRes.value !== null) {
        visible = true;
      }
    }
  }
  if (!visible) {
    return err(404, 'item_not_found', 'item not found');
  }

  let plaid;
  try {
    plaid = await getPlaidService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const result = await plaid.removeItem({ itemId, reason: parsed.data.reason });
  if (!result.ok) {
    if (result.error.kind === 'item_not_found') {
      return err(404, 'item_not_found', 'item not found');
    }
    return err(502, 'plaid_api_error', 'plaid call failed');
  }

  const body = PlaidItemRemoveResponseSchema.parse({ ok: true });
  return NextResponse.json(body, { status: 200 });
}
