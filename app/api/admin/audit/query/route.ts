// Greylock — GET /api/admin/audit/query
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). Owner-only audit-log query. Returns matching
// `AuditEntry` rows with `prevHash`/`entryHash` rendered as base64 strings
// (the wire shape — the on-disk shape is raw bytes).
//
// Auth: session + User.role === 'owner'. Non-owner → 403.
// Hard bound: `limit` ≤ 1000 — server-side cap; the request is rejected at
// 400 if a higher value is sent. The repo also enforces a default.
//
// Sanitizer guarantee: this route NEVER includes token-shape data in the
// response. Bytes are base64 (a fixed 32-byte hash), `detailsJson` is the
// already-sanitized payload from the audit append site.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';
import { z } from 'zod';

import { SessionId, UserId } from '../../../../../lib/types/domain.js';
import type { AuditAction, Domain } from '../../../../../lib/types/domain.js';
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

// Allowed values for `action` and `domain` mirror the domain enums. We
// accept any string and the repo's WHERE clause matches by literal — but
// we still gate the input at the Zod boundary so a route-layer attacker
// can't smuggle SQL.
const QuerySchema = z.object({
  fromTs: z.string().datetime().optional(),
  toTs: z.string().datetime().optional(),
  actorUserId: z.string().min(1).max(64).optional(),
  action: z.string().min(1).max(80).optional(),
  domain: z.enum(['personal', 'pcc']).optional(),
  // Hard cap: any number ≤ 1000. Lower bound ≥ 1.
  limit: z.coerce.number().int().min(1).max(1000).optional(),
});

interface AuditEntryWire {
  readonly seq: string; // BigInt → string
  readonly ts: string;
  readonly tsNanos: number;
  readonly actorUserId: string | null;
  readonly actorKind: string;
  readonly domain: string | null;
  readonly subjectId: string | null;
  readonly subjectKind: string | null;
  readonly action: string;
  readonly outcome: string;
  readonly detailsJson: string;
  readonly prevHash: string; // base64
  readonly entryHash: string; // base64
}

function toBase64(bytes: Uint8Array): string {
  return Buffer.from(bytes).toString('base64');
}

export async function GET(req: NextRequest): Promise<NextResponse> {
  // 1. Session.
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

  // 2. Owner gate.
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

  // 3. Query parameters.
  const url = new URL(req.url);
  const raw: Record<string, string> = {};
  for (const [k, v] of url.searchParams.entries()) {
    // `k` comes from URLSearchParams iteration; we write into a fresh
    // object that's then handed to Zod for validation. No prototype-pollution
    // risk — `Record` literal has Object.prototype, and any '__proto__' key
    // would just be a plain string property post-Zod-parse.
    // eslint-disable-next-line security/detect-object-injection
    raw[k] = v;
  }
  const parsed = QuerySchema.safeParse(raw);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'query parameters did not validate');
  }

  // 4. Run the query.
  let audit;
  try {
    audit = await getAuditService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const result = await audit.query({
    ...(parsed.data.fromTs !== undefined ? { fromTs: new Date(parsed.data.fromTs) } : {}),
    ...(parsed.data.toTs !== undefined ? { toTs: new Date(parsed.data.toTs) } : {}),
    ...(parsed.data.actorUserId !== undefined
      ? { actorUserId: UserId(parsed.data.actorUserId) }
      : {}),
    ...(parsed.data.action !== undefined ? { action: parsed.data.action as AuditAction } : {}),
    ...(parsed.data.domain !== undefined ? { domain: parsed.data.domain as Domain } : {}),
    limit: parsed.data.limit ?? 1000,
  });
  if (!result.ok) {
    return err(500, 'storage_failure', 'unable to query audit log');
  }

  // 5. Render to wire shape (BigInt → string, bytes → base64).
  const entries: AuditEntryWire[] = result.value.map((e) => ({
    seq: e.seq.toString(),
    ts: e.ts.toISOString(),
    tsNanos: e.tsNanos,
    actorUserId: e.actorUserId,
    actorKind: e.actorKind,
    domain: e.domain,
    subjectId: e.subjectId,
    subjectKind: e.subjectKind,
    action: e.action,
    outcome: e.outcome,
    detailsJson: e.detailsJson,
    prevHash: toBase64(e.prevHash),
    entryHash: toBase64(e.entryHash),
  }));

  return NextResponse.json({ entries }, { status: 200 });
}
