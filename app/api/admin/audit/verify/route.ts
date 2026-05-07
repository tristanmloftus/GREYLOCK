// Greylock — GET /api/admin/audit/verify
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). Owner-only audit-chain integrity check.
// Mirrors `pnpm admin:audit-verify` (which AGENT-DB / Orchestrator wires
// in `scripts/admin-audit-verify.ts`).
//
// Auth: session + User.role === 'owner'. Anything else returns 403.
// Audit: emits `admin_audit_verify_invoked` regardless of chain outcome.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  ActorKind,
  AuditAction,
  AuditOutcome,
  SessionId,
} from '../../../../../lib/types/domain.js';
import { AdminAuditVerifyResponseSchema } from '../../../../../lib/types/zod-schemas.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../../lib/auth/session.js';
import {
  getAuditService,
  getAuthService,
  getRepos,
} from '../../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

export async function GET(req: NextRequest): Promise<NextResponse> {
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
  const sessionId = SessionId(unsealed.value.sessionId);
  const validated = await auth.validateSession({ sessionId, cookieValue: cookie.value });
  if (!validated.ok) {
    return err(401, 'invalid_session', 'session invalid');
  }

  // Owner-gate: read the User row, check role.
  let repos;
  try {
    repos = await getRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const userResult = await repos.userRepo.findById(validated.value.userId);
  if (!userResult.ok || userResult.value === null) {
    return err(401, 'invalid_session', 'session invalid');
  }
  if (userResult.value.role !== 'owner') {
    return err(403, 'unauthorized', 'owner role required');
  }
  const ownerUserId = userResult.value.id;

  // Run the verification.
  let audit;
  try {
    audit = await getAuditService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const result = await audit.verifyChain();

  // Emit `admin_audit_verify_invoked`.
  await audit.append({
    actorUserId: ownerUserId,
    actorKind: ActorKind.AdminCli,
    domain: null,
    subjectId: null,
    subjectKind: null,
    action: AuditAction.AdminAuditVerifyInvoked,
    outcome: result.ok ? AuditOutcome.Success : AuditOutcome.Failure,
    details: result.ok
      ? { verifiedCount: result.value.verifiedCount }
      : { reason: 'chain_break_or_storage_failure' },
  });

  if (!result.ok) {
    if (result.error.kind === 'chain_break') {
      const body = AdminAuditVerifyResponseSchema.parse({
        verifiedCount: 0,
        brokenAtSeq: result.error.atSeq.toString(),
      });
      return NextResponse.json(body, { status: 200 });
    }
    return err(500, 'storage_failure', 'unable to verify chain');
  }

  const body = AdminAuditVerifyResponseSchema.parse({
    verifiedCount: result.value.verifiedCount,
    brokenAtSeq: null,
  });
  return NextResponse.json(body, { status: 200 });
}
