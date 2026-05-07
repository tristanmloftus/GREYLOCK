// Greylock — GET /api/dashboard/series
// =============================================================================
// AGENT-UI (Phase 4). Time-series of NetWorthSnapshot rows in [fromTs, toTs].
// Cents serialized as strings.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  DashboardSeriesQuerySchema,
  DashboardSeriesResponseSchema,
} from '../../../../lib/types/zod-schemas.js';
import { SessionId } from '../../../../lib/types/domain.js';
import type { Domain } from '../../../../lib/types/domain.js';
import type { RepoScope, SnapshotRepository } from '../../../../lib/types/services.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
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
  // 1. Query.
  const url = new URL(req.url);
  const raw: Record<string, string> = {};
  for (const [k, v] of url.searchParams.entries()) {
    // eslint-disable-next-line security/detect-object-injection
    raw[k] = v;
  }
  const parsed = DashboardSeriesQuerySchema.safeParse(raw);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'query parameters did not validate');
  }
  const domain: Domain = parsed.data.domain;

  // 2. Session.
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
  const sess = await auth.validateSession({
    sessionId: SessionId(unsealed.value.sessionId),
    cookieValue: cookie.value,
  });
  if (!sess.ok) {
    return err(401, 'invalid_session', 'session invalid');
  }
  const userId = sess.value.userId;

  // 3. PCC gate.
  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  let scope: RepoScope;
  if (domain === 'pcc') {
    const member = await repos.pccMembershipRepo.isActiveMember(userId);
    if (!member.ok || !member.value) {
      return err(403, 'unauthorized', 'pcc membership required');
    }
    scope = { kind: 'pcc', memberOfUserId: userId };
  } else {
    scope = { kind: 'personal', userId };
  }

  // 4. Snapshot repo.
  type DbModShape = {
    readonly getBootedDb?: () => {
      readonly repos: {
        readonly snapshotRepo: SnapshotRepository;
      };
    };
  };
  let snapshotRepo: SnapshotRepository | null = null;
  try {
    const dbPath = '../../../../lib/db/index.js';
    const dbMod = (await import(/* @vite-ignore */ dbPath)) as DbModShape;
    if (dbMod.getBootedDb !== undefined) {
      snapshotRepo = dbMod.getBootedDb().repos.snapshotRepo;
    }
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  if (snapshotRepo === null) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }

  const seriesRes = await snapshotRepo.series(scope, {
    domain,
    userId: domain === 'personal' ? userId : null,
    fromTs: new Date(parsed.data.fromTs),
    toTs: new Date(parsed.data.toTs),
  });
  if (!seriesRes.ok) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }

  const body = DashboardSeriesResponseSchema.parse({
    domain,
    points: seriesRes.value.map((s) => ({
      takenAt: s.takenAt.toISOString(),
      netWorthCents: s.netWorthCents,
      cashCents: s.cashCents,
    })),
  });
  return NextResponse.json(body, { status: 200 });
}
