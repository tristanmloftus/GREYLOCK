// Greylock — POST /api/sync/run
// =============================================================================
// AGENT-SYNC (Phase 3). Manual sync trigger for a specific item.
//
// Auth: session. PCC items additionally require active PccMembership; the
// SyncOrchestrator enforces this implicitly by gating the lookup through the
// caller's repo scope.
//
// Body: { itemId } (Zod-validated against SyncTriggerRequestSchema).
// Response: SyncTriggerResponseSchema { added, modified, removed, hasMore }.
//
// Resolves the orchestrator from the services-registry; mocks in tests via
// the `__setRegistryOverridesForTests` seam (Phase 5 will tighten under
// production guard — see QA-SEC-phase-2.md M-3).
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  SyncTriggerRequestSchema,
  SyncTriggerResponseSchema,
} from '../../../../lib/types/zod-schemas.js';
import { ItemId, SessionId } from '../../../../lib/types/domain.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
import { getAuthService } from '../../../../lib/runtime/services-registry.js';
import { getSyncOrchestrator } from './services.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  // Body validation.
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = SyncTriggerRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // Session cookie.
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
  const sessionId = SessionId(unsealed.value.sessionId);

  // Validate session (sliding-window).
  let auth;
  try {
    auth = await getAuthService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const sess = await auth.validateSession({
    sessionId,
    cookieValue: cookie.value,
  });
  if (!sess.ok) {
    return err(401, 'session_expired', 'session expired or invalid');
  }
  const initiatorUserId = sess.value.userId;

  // Resolve the orchestrator (dynamic import — falls back to 503 if not wired).
  let orchestrator;
  try {
    orchestrator = await getSyncOrchestrator();
  } catch {
    return err(503, 'service_unavailable', 'sync orchestrator unavailable');
  }

  const result = await orchestrator.syncItem({
    itemId: ItemId(parsed.data.itemId),
    initiatorUserId,
  });
  if (!result.ok) {
    if (result.error.kind === 'keybridge_unavailable' || result.error.kind === 'crypto_unavailable') {
      return err(503, result.error.kind, 'service temporarily unavailable');
    }
    if (result.error.kind === 'unexpected' && result.error.cause === 'item_not_found') {
      return err(404, 'item_not_found', 'item not found or out of scope');
    }
    return err(502, 'plaid_sync_failed', 'sync failed');
  }

  const responseBody = SyncTriggerResponseSchema.parse({
    added: result.value.added,
    modified: result.value.modified,
    removed: result.value.removed,
    hasMore: result.value.hasMore,
  });
  return NextResponse.json(responseBody, { status: 200 });
}
