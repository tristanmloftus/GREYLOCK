// Greylock — GET /api/plaid/items
// =============================================================================
// AGENT-PLAID (Phase 3). List items visible under the caller's scope. Optional
// `?domain=personal|pcc` query param. PCC reads gated by membership.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { PlaidItemListResponseSchema } from '../../../../lib/types/zod-schemas.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
import { SessionId } from '../../../../lib/types/domain.js';
import type { Domain, Item } from '../../../../lib/types/domain.js';
import type { RepoScope } from '../../../../lib/types/services.js';
import {
  getAuthService,
  getFullRepos,
} from '../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function GET(req: NextRequest): Promise<NextResponse> {
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

  // Optional domain filter.
  const url = new URL(req.url);
  const domainParam = url.searchParams.get('domain');
  let domain: Domain | undefined;
  if (domainParam === 'personal') {
    domain = 'personal';
  } else if (domainParam === 'pcc') {
    domain = 'pcc';
  } else if (domainParam !== null) {
    return err(400, 'invalid_request', 'invalid domain');
  }

  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }

  // Gather items via personal scope, plus pcc if member.
  const items: Item[] = [];
  if (domain === 'personal' || domain === undefined) {
    const personalScope: RepoScope = { kind: 'personal', userId };
    const personalRes = await repos.itemRepo.list(personalScope);
    if (!personalRes.ok) {
      return err(503, 'service_unavailable', 'service temporarily unavailable');
    }
    items.push(...personalRes.value);
  }
  if (domain === 'pcc' || domain === undefined) {
    const memberRes = await repos.pccMembershipRepo.isActiveMember(userId);
    if (memberRes.ok && memberRes.value) {
      const pccScope: RepoScope = { kind: 'pcc', memberOfUserId: userId };
      const pccRes = await repos.itemRepo.list(pccScope);
      if (pccRes.ok) {
        items.push(...pccRes.value);
      }
    } else if (domain === 'pcc') {
      return err(403, 'unauthorized', 'pcc membership required');
    }
  }

  const body = PlaidItemListResponseSchema.parse({
    items: items.map((it) => ({
      itemId: it.id,
      domain: it.domain,
      institutionName: it.institutionName,
      lastSyncAt: it.lastSyncAt === null ? null : it.lastSyncAt.toISOString(),
      lastSyncOutcome: it.lastSyncOutcome,
      consecutiveFailures: it.consecutiveFailures,
      removedAt: it.removedAt === null ? null : it.removedAt.toISOString(),
    })),
  });
  return NextResponse.json(body, { status: 200 });
}
