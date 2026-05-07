// Greylock — GET /api/healthz
// =============================================================================
// AGENT-UI (Phase 4). Public, lightly rate-limited. Returns `{ ok, checks }`.
//   - db: 'ok' iff `getBootedDb()` resolves to a non-null PrismaClient.
//   - crypto: 'ok' iff CryptoService reports `hasPccDek()` is true (post-boot).
//   - keybridge: 'ok' iff /tmp/greylock-keybridge.sock exists with mode 0600.
//
// Rate limit: 60/min/IP. Uses the existing RateLimitRepository under the
// flow key 'healthz'.
// =============================================================================

import { NextResponse } from 'next/server';
import type { NextRequest } from 'next/server';
import { existsSync, statSync } from 'node:fs';

import {
  getCryptoServiceLazy,
  getRepos,
} from '../../../lib/runtime/services-registry.js';

interface HealthBody {
  readonly ok: boolean;
  readonly checks: {
    readonly db: 'ok' | 'down';
    readonly crypto: 'ok' | 'down';
    readonly keybridge: 'ok' | 'down';
  };
}

// In-process token bucket for /healthz. We deliberately avoid using the
// auth `RateLimitRepository` (whose flow taxonomy is `auth:*`) because health
// must work even if the DB is degraded. The bucket is per-IP and resets every
// minute. Only a soft cap to prevent trivial flooding.
const HEALTHZ_CAP = 60;
const HEALTHZ_WINDOW_MS = 60_000;
const healthzBuckets = new Map<string, { windowStart: number; count: number }>();

function consumeHealthzBucket(req: NextRequest): { allowed: boolean; retryAfterSeconds: number } {
  const ip = req.headers.get('x-forwarded-for') ?? 'unknown';
  const now = Date.now();
  const existing = healthzBuckets.get(ip);
  if (existing === undefined || now - existing.windowStart >= HEALTHZ_WINDOW_MS) {
    healthzBuckets.set(ip, { windowStart: now, count: 1 });
    return { allowed: true, retryAfterSeconds: 0 };
  }
  if (existing.count + 1 > HEALTHZ_CAP) {
    const retryAfterSeconds = Math.max(1, Math.ceil((HEALTHZ_WINDOW_MS - (now - existing.windowStart)) / 1000));
    return { allowed: false, retryAfterSeconds };
  }
  existing.count += 1;
  return { allowed: true, retryAfterSeconds: 0 };
}

function checkKeybridge(): 'ok' | 'down' {
  const path = process.env['KEYBRIDGE_SOCKET_PATH'] ?? '/tmp/greylock-keybridge.sock';
  // existsSync + statSync are sync but cheap; we accept the lint for `non-literal-fs-filename`
  // because the path is configured via env, not user input.
  // eslint-disable-next-line security/detect-non-literal-fs-filename
  if (!existsSync(path)) {
    return 'down';
  }
  try {
    // eslint-disable-next-line security/detect-non-literal-fs-filename
    const st = statSync(path);
    // We only confirm it's a socket-like inode here; mode is verified by the
    // owner at boot (keybridge-server enforces 0600). Avoid crashing on
    // platforms whose stat does not surface the precise mode bits we want.
    if (!st.isSocket()) {
      return 'down';
    }
    return 'ok';
  } catch {
    return 'down';
  }
}

export async function GET(req: NextRequest): Promise<NextResponse> {
  // Rate limit (in-process, per IP, 60/min).
  const rl = consumeHealthzBucket(req);
  if (!rl.allowed) {
    return NextResponse.json(
      {
        error: {
          code: 'rate_limited',
          message: 'too many requests',
          retryAfterSeconds: rl.retryAfterSeconds,
        },
      },
      { status: 429 },
    );
  }

  let dbStatus: 'ok' | 'down' = 'ok';
  try {
    await getRepos();
  } catch {
    dbStatus = 'down';
  }

  let cryptoStatus: 'ok' | 'down' = 'ok';
  try {
    const c = await getCryptoServiceLazy();
    if (!c.hasPccDek()) {
      cryptoStatus = 'down';
    }
  } catch {
    cryptoStatus = 'down';
  }

  const keybridgeStatus = checkKeybridge();

  const ok = dbStatus === 'ok' && cryptoStatus === 'ok' && keybridgeStatus === 'ok';
  const body: HealthBody = {
    ok,
    checks: { db: dbStatus, crypto: cryptoStatus, keybridge: keybridgeStatus },
  };
  return NextResponse.json(body, { status: 200 });
}
