// Greylock — GET /api/dashboard/snapshot
// =============================================================================
// AGENT-UI (Phase 4). Read-through over the latest NetWorthSnapshot for the
// caller's scope. PCC requires PccMembership. Cents are returned as strings
// (CentsOutSchema) to avoid JSON-number precision loss.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import { DashboardSnapshotResponseSchema } from '../../../../lib/types/zod-schemas.js';
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

const GOAL = 100_000_000_000n; // $1B

export async function GET(req: NextRequest): Promise<NextResponse> {
  // 1. Domain query.
  const url = new URL(req.url);
  const domainParam = url.searchParams.get('domain');
  let domain: Domain;
  if (domainParam === 'personal') {
    domain = 'personal';
  } else if (domainParam === 'pcc') {
    domain = 'pcc';
  } else {
    return err(400, 'invalid_request', 'invalid domain');
  }

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

  // 3. Resolve repos + verify PCC membership where relevant.
  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  let scope: RepoScope;
  if (domain === 'pcc') {
    const memberRes = await repos.pccMembershipRepo.isActiveMember(userId);
    if (!memberRes.ok || !memberRes.value) {
      return err(403, 'unauthorized', 'pcc membership required');
    }
    scope = { kind: 'pcc', memberOfUserId: userId };
  } else {
    scope = { kind: 'personal', userId };
  }

  // 4. Latest snapshot.
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
  const snapRes = await snapshotRepo.latest(scope, {
    domain,
    userId: domain === 'personal' ? userId : null,
  });
  if (!snapRes.ok) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }

  // 5. Build response. If no snapshot, return zero-values (UI shows empty state).
  const snap = snapRes.value;
  let assetsCents = 0n;
  let liabilitiesCents = 0n;
  let netWorthCents = 0n;
  let cashCents = 0n;
  let monthNetCents: bigint | null = null;
  const breakdown: Array<{
    accountId: string;
    name: string;
    type: 'depository' | 'credit' | 'loan' | 'investment' | 'other';
    balanceCents: bigint;
    contribution: 'asset' | 'liability';
  }> = [];
  let takenAt = new Date();
  if (snap !== null) {
    assetsCents = snap.assetsCents;
    liabilitiesCents = snap.liabilitiesCents;
    netWorthCents = snap.netWorthCents;
    cashCents = snap.cashCents;
    monthNetCents = snap.monthNetCents;
    takenAt = snap.takenAt;
    // The snapshot stores its breakdown JSON; decode if present.
    try {
      const parsedBreakdown = JSON.parse(snap.breakdownJson) as ReadonlyArray<{
        accountId: string;
        name: string;
        type: 'depository' | 'credit' | 'loan' | 'investment' | 'other';
        balanceCents: string;
        contribution: 'asset' | 'liability';
      }>;
      for (const b of parsedBreakdown) {
        breakdown.push({
          accountId: b.accountId,
          name: b.name,
          type: b.type,
          balanceCents: BigInt(b.balanceCents),
          contribution: b.contribution,
        });
      }
    } catch {
      // Malformed breakdown — leave empty rather than 500.
    }
  }

  const billionProgress = Math.min(1, Math.max(0, Number(netWorthCents) / Number(GOAL)));

  const body = DashboardSnapshotResponseSchema.parse({
    domain,
    takenAt: takenAt.toISOString(),
    assetsCents,
    liabilitiesCents,
    netWorthCents,
    cashCents,
    monthNetCents,
    billionProgress,
    breakdown,
  });
  return NextResponse.json(body, { status: 200 });
}
