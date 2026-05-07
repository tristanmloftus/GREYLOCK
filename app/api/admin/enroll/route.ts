// Greylock — POST /api/admin/enroll
// =============================================================================
// AGENT-UI (Phase 4). Owner-only. Mints an enrollment URL token (mirroring
// `pnpm admin:enroll`). Audits `admin_enroll_invoked`.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';

import {
  AdminEnrollRequestSchema,
  AdminEnrollResponseSchema,
} from '../../../../lib/types/zod-schemas.js';
import {
  ActorKind,
  AuditAction,
  AuditOutcome,
  SessionId,
} from '../../../../lib/types/domain.js';
import type { UserId } from '../../../../lib/types/domain.js';
import type { PrismaClient } from '@prisma/client';
import {
  isAllowedEmail,
  isPlaceholderEmail,
  normalizeEmail,
} from '../../../../lib/auth/allowlist.js';
import {
  readSessionConfig,
  unsealSessionCookie,
} from '../../../../lib/auth/session.js';
import {
  getAuditService,
  getAuthService,
  getRepos,
} from '../../../../lib/runtime/services-registry.js';

interface ErrorBody {
  readonly error: { readonly code: string; readonly message: string };
}

function err(status: number, code: string, message: string): NextResponse {
  const body: ErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status });
}

const ENROLLMENT_TTL_MIN = 30;

export async function POST(req: NextRequest): Promise<NextResponse> {
  // 1. Body.
  let json: unknown;
  try {
    json = await req.json();
  } catch {
    return err(400, 'invalid_json', 'request body must be JSON');
  }
  const parsed = AdminEnrollRequestSchema.safeParse(json);
  if (!parsed.success) {
    return err(400, 'invalid_request', 'request body did not validate');
  }

  // 2. Session.
  const sessionConfig = readSessionConfig();
  const cookie = req.cookies.get(sessionConfig.cookieName);
  if (cookie === undefined) {
    return err(404, 'not_found', 'route not found');
  }
  const unsealed = await unsealSessionCookie({ cookieValue: cookie.value, config: sessionConfig });
  if (!unsealed.ok) {
    return err(404, 'not_found', 'route not found');
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
    return err(404, 'not_found', 'route not found');
  }

  // 3. Owner gate.
  let repos;
  try {
    repos = await getRepos();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const callerRes = await repos.userRepo.findById(sess.value.userId);
  if (!callerRes.ok || callerRes.value === null) {
    return err(404, 'not_found', 'route not found');
  }
  if (callerRes.value.role !== 'owner') {
    return err(404, 'not_found', 'route not found');
  }
  const ownerId = callerRes.value.id;

  // 4. Allowlist + placeholder gates.
  const email = normalizeEmail(parsed.data.email);
  if (isPlaceholderEmail(email)) {
    return err(400, 'placeholder_email_rejected', 'placeholder email cannot enroll');
  }
  if (!isAllowedEmail(email)) {
    return err(400, 'email_not_allowlisted', 'email not in allowlist');
  }
  if (parsed.data.role === 'owner') {
    const ownerEmailEnv = (process.env['OWNER_EMAIL'] ?? '').trim().toLowerCase();
    if (email !== ownerEmailEnv) {
      return err(400, 'email_not_allowlisted', 'owner role only for OWNER_EMAIL');
    }
  }

  // 5. Upsert User row.
  const existing = await repos.userRepo.findByEmail(email);
  if (!existing.ok) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  let userId: UserId;
  if (existing.value !== null) {
    userId = existing.value.id;
  } else {
    const created = await repos.userRepo.create({
      email,
      displayName: parsed.data.displayName,
      role: parsed.data.role,
    });
    if (!created.ok) {
      return err(503, 'service_unavailable', 'service temporarily unavailable');
    }
    userId = created.value.id;
  }

  // 6. Mint token via the booted-DB helper (lib/db/index.ts exports
  //    `mintEnrollmentToken` and the booted Prisma client lives behind
  //    `getBootedDb()`).
  type DbModShape = {
    readonly getBootedDb?: () => { readonly prisma: PrismaClient };
    readonly mintEnrollmentToken?: (args: {
      readonly prisma: PrismaClient;
      readonly email: string;
      readonly ttlMinutes?: number;
    }) => Promise<
      | { readonly ok: true; readonly value: { readonly tokenId: string; readonly cleartextToken: string; readonly email: string; readonly expiresAt: Date } }
      | { readonly ok: false; readonly error: { readonly kind: string } }
    >;
  };
  let dbMod: DbModShape | null = null;
  try {
    const dbPath = '../../../../lib/db/index.js';
    dbMod = (await import(/* @vite-ignore */ dbPath)) as DbModShape;
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  if (dbMod === null || dbMod.getBootedDb === undefined || dbMod.mintEnrollmentToken === undefined) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  const minted = await dbMod.mintEnrollmentToken({
    prisma: dbMod.getBootedDb().prisma,
    email,
    ttlMinutes: ENROLLMENT_TTL_MIN,
  });
  if (!minted.ok) {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }

  // 7. Audit.
  let audit;
  try {
    audit = await getAuditService();
  } catch {
    return err(503, 'service_unavailable', 'service temporarily unavailable');
  }
  await audit.append({
    actorUserId: ownerId,
    actorKind: ActorKind.AdminCli,
    domain: null,
    subjectId: userId,
    subjectKind: 'user',
    action: AuditAction.AdminEnrollInvoked,
    outcome: AuditOutcome.Success,
    details: { email, role: parsed.data.role, ttlMinutes: ENROLLMENT_TTL_MIN, via: 'http' },
  });

  // 8. Response.
  const appUrl = process.env['APP_URL'] ?? 'https://localhost:3000';
  const url = `${appUrl}/auth/enroll?email=${encodeURIComponent(email)}&token=${minted.value.cleartextToken}`;
  const body = AdminEnrollResponseSchema.parse({
    enrollmentUrl: url,
    expiresAt: minted.value.expiresAt.toISOString(),
  });
  return NextResponse.json(body, { status: 200 });
}
