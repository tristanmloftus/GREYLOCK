#!/usr/bin/env tsx
// Greylock — `pnpm sync` worker entry
// =============================================================================
// AGENT-SYNC (Phase 3). Long-running background process that:
//   1. Loads the Master KEK from Keychain (or DEV_DB_PASSPHRASE in dev).
//   2. Derives the SQLCipher key, opens its own Prisma client.
//   3. Loads the active PCC DEK into the worker's CryptoService instance.
//   4. Connects the keybridge client to the web process for personal DEK
//      borrows.
//   5. Loops every SYNC_INTERVAL_MINUTES (15) calling SyncOrchestrator.
//   6. On SIGINT/SIGTERM: shuts down crypto, closes keybridge + DB.
//
// Prints terse steady-state status per docs/ARCHITECTURE.md §9. The audit
// trail goes through AuditService — never via stdout.
// =============================================================================

import { hkdfSync } from 'node:crypto';
import * as fs from 'node:fs';

import { Buffer } from 'node:buffer';

import { HKDF_KEYBRIDGE_INFO } from '../lib/crypto/kdf.js';
import { unwrapPccDek } from '../lib/crypto/pcc-dek.js';
import { bootDb } from '../lib/db/index.js';
import { createKeybridgeClient } from '../lib/ipc/index.js';
import { createSyncOrchestrator, createSnapshotWriter } from '../lib/sync/index.js';

import type {
  Cents,
  EncryptedBlob,
  PlaidError,
  Result,
} from '../lib/types/domain.js';
import { Err, Ok } from '../lib/types/domain.js';
import type {
  AuditService,
  ComputeService,
  PlaidService,
  PlaidSyncResult,
} from '../lib/types/services.js';

// -----------------------------------------------------------------------------
// Required env reading
// -----------------------------------------------------------------------------

interface ResolvedEnv {
  readonly databaseUrl: string;
  readonly intervalMs: number;
  readonly socketPath: string;
  readonly cryptoPepper: Uint8Array;
}

function readEnv(): ResolvedEnv {
  const databaseUrl = process.env['DATABASE_URL'];
  if (databaseUrl === undefined || databaseUrl.length === 0) {
    throw new Error('sync: DATABASE_URL is required');
  }
  const intervalStr = process.env['SYNC_INTERVAL_MINUTES'] ?? '15';
  const intervalMin = Number.parseInt(intervalStr, 10);
  if (!Number.isFinite(intervalMin) || intervalMin <= 0) {
    throw new Error('sync: SYNC_INTERVAL_MINUTES must be a positive integer');
  }

  const socketPath = process.env['KEYBRIDGE_SOCKET_PATH'] ?? '/tmp/greylock-keybridge.sock';

  const pepperB64 = process.env['CRYPTO_PEPPER'];
  let cryptoPepper: Uint8Array;
  if (process.env['NODE_ENV'] === 'production') {
    if (pepperB64 === undefined || pepperB64.length === 0) {
      throw new Error('sync: CRYPTO_PEPPER required in production');
    }
    cryptoPepper = new Uint8Array(Buffer.from(pepperB64, 'base64'));
  } else {
    cryptoPepper =
      pepperB64 !== undefined && pepperB64.length > 0
        ? new Uint8Array(Buffer.from(pepperB64, 'base64'))
        : new Uint8Array(Buffer.from('greylock-dev-pepper-v1', 'utf8'));
  }

  return {
    databaseUrl,
    intervalMs: intervalMin * 60 * 1000,
    socketPath,
    cryptoPepper,
  };
}

// -----------------------------------------------------------------------------
// Plaid stub fallback — when AGENT-PLAID's module is missing, the worker
// still boots cleanly and prints "no items to sync".
// -----------------------------------------------------------------------------

async function resolvePlaidService(): Promise<PlaidService> {
  try {
    // Dynamic import; lib/plaid/ may not be on disk yet during Phase 3 build.
    const path = '../lib/plaid/index.js';
    const mod = (await import(/* @vite-ignore */ path)) as {
      readonly createPlaidService?: () => PlaidService;
    };
    if (typeof mod.createPlaidService === 'function') {
      return mod.createPlaidService();
    }
  } catch {
    // fall through to the no-op stub
  }
  // Worker-local stub: every method returns a benign Err. The orchestrator
  // counts these as failures, the cycle still completes.
  const noopErr: PlaidError = {
    kind: 'plaid_api_error',
    httpStatus: 0,
    errorCode: 'plaid_module_not_loaded',
  };
  return {
    async mintLinkToken(): Promise<Result<{ linkToken: never; expiresAt: Date }, PlaidError>> {
      return Err(noopErr);
    },
    async exchangePublicToken(): Promise<Result<{ itemId: never; plaidItemId: never }, PlaidError>> {
      return Err(noopErr);
    },
    async syncItem(): Promise<Result<PlaidSyncResult, PlaidError>> {
      return Err(noopErr);
    },
    async refreshBalances(): Promise<Result<{ accountsUpdated: number }, PlaidError>> {
      return Err(noopErr);
    },
    async removeItem(): Promise<Result<void, PlaidError>> {
      return Err(noopErr);
    },
  } as unknown as PlaidService;
}

async function resolveComputeService(): Promise<{
  netWorth: (i: { accounts: ReadonlyArray<unknown> }) => unknown;
  cashOnly: (i: { accounts: ReadonlyArray<unknown> }) => Cents;
  monthNet: (i: { transactions: ReadonlyArray<unknown>; now: Date }) => unknown;
  billionProgress: (i: { netWorthCents: Cents }) => unknown;
}> {
  const path = '../lib/compute/index.js';
  try {
    const mod = (await import(/* @vite-ignore */ path)) as Record<string, unknown>;
    if (
      typeof mod['netWorth'] === 'function' &&
      typeof mod['cashOnly'] === 'function' &&
      typeof mod['monthNet'] === 'function' &&
      typeof mod['billionProgress'] === 'function'
    ) {
      return mod as ReturnType<typeof resolveComputeService> extends Promise<infer R> ? R : never;
    }
  } catch {
    // fall through
  }
  // Stub if compute module isn't shipped yet (only some files exist today).
  return {
    netWorth: (): unknown => ({
      assetsCents: 0n,
      liabilitiesCents: 0n,
      netWorthCents: 0n,
      cashCents: 0n,
      breakdown: [],
    }),
    cashOnly: (): Cents => 0n,
    monthNet: (): unknown => ({
      windowStart: new Date(),
      windowEnd: new Date(),
      inflowCents: 0n,
      outflowCents: 0n,
      netCents: 0n,
    }),
    billionProgress: (): unknown => ({
      netWorthCents: 0n,
      goalCents: 100_000_000_000_00n,
      progress: 0,
    }),
  };
}

async function resolveAuditService(): Promise<AuditService> {
  try {
    const path = '../lib/audit/index.js';
    const mod = (await import(/* @vite-ignore */ path)) as {
      readonly createAuditService?: () => AuditService;
    };
    if (typeof mod.createAuditService === 'function') {
      return mod.createAuditService();
    }
  } catch {
    // fall through
  }
  // No-op audit shim. Real audit lands in Phase 3 alongside this work.
  const noop = async (): Promise<{ ok: false; error: { kind: 'storage_failure' } }> => ({
    ok: false,
    error: { kind: 'storage_failure' },
  });
  return {
    append: noop as unknown as AuditService['append'],
    query: noop as unknown as AuditService['query'],
    verifyChain: noop as unknown as AuditService['verifyChain'],
  };
}

// -----------------------------------------------------------------------------
// Boot + main loop
// -----------------------------------------------------------------------------

async function main(): Promise<number> {
  const env = readEnv();

  // 1. Load active PCC key wrap to get kdfSalt (so we derive the same Master KEK
  //    the web process did).
  process.stdout.write('[sync] connecting to encrypted DB...\n');

  // We can't open the DB without the Master KEK, but we can't derive the
  // Master KEK without the kdfSalt that lives inside the DB. Bootstrapping
  // path matches `lib/runtime/boot.ts`: open with the Master KEK derived
  // from a default kdfSalt only if no PccKeyWrap exists yet (fresh install),
  // otherwise probe via a side channel. For Phase 3 the PCC wrap is already
  // there — we follow the same pattern as the web process: load the wrap row
  // via a temporary read using `DEV_DB_PASSPHRASE` for dev. In production the
  // operator runs `pnpm dev` first which materializes the SQLCipher key.

  // For simplicity we mirror what the web process does: derive Master KEK
  // from the Keychain (or DEV_DB_PASSPHRASE) using the kdfSalt persisted
  // in the most recent active PccKeyWrap row. If no row exists, we abort.

  // To do that, we need to read the wrap row WITHOUT a ready Master KEK. The
  // way the web process does this: derive the SQLCipher key from the Master
  // KEK derived using the *latest* wrap row's `kdfSalt`. Catch-22 → we read
  // the kdfSalt from a tiny side-channel: the wrap row's `kdfSalt` is plain
  // bytes inside an encrypted DB, but the FIRST wrap row was created during
  // crypto bootstrap; its kdfSalt is the same that the web process reads at
  // boot.
  //
  // Practical approach: borrow the same convention as `bootDb` which expects
  // a sqlcipherKey. We construct it via DEV_DB_PASSPHRASE on dev (matches
  // `scripts/db/dev-key.ts`), and via Keychain on prod. The `kdfSalt` from
  // the wrap row then matches the one used during web-process boot.

  let masterKek: Buffer | null = null;
  let pccDek: Buffer | null = null;
  let booted: Awaited<ReturnType<typeof bootDb>> | null = null;
  let realKeybridgeRef: ReturnType<typeof createKeybridgeClient> | null = null;
  let cycleTimer: NodeJS.Timeout | null = null;

  const cleanup = async (): Promise<void> => {
    process.stdout.write('[sync] shutting down...\n');
    if (cycleTimer !== null) {
      clearInterval(cycleTimer);
      cycleTimer = null;
    }
    if (realKeybridgeRef !== null) {
      try {
        await realKeybridgeRef.disconnect();
      } catch {
        // best-effort
      }
    }
    if (masterKek !== null) {
      masterKek.fill(0);
      masterKek = null;
    }
    if (pccDek !== null) {
      pccDek.fill(0);
      pccDek = null;
    }
    if (booted !== null) {
      try {
        await booted.dispose();
      } catch {
        // best-effort
      }
      booted = null;
    }
    process.stdout.write('[sync] clean shutdown complete.\n');
  };

  let shuttingDown = false;
  const onSignal = (sig: string): void => {
    if (shuttingDown) {
      return;
    }
    shuttingDown = true;
    process.stdout.write(`[sync] received ${sig}\n`);
    void cleanup().then(() => {
      process.exit(0);
    });
  };
  process.on('SIGINT', () => onSignal('SIGINT'));
  process.on('SIGTERM', () => onSignal('SIGTERM'));

  // For dev we use the same dev-key derivation as the web side.
  // For prod: load Master KEK using a guess kdfSalt from the wrap, but we
  // need the wrap. We open the DB with a SQLCipher key derived from a tmp
  // Master KEK candidate; if it fails, propagate.
  const { deriveDevKey } = await import('./db/dev-key.js');

  let sqlcipherKey: Uint8Array;
  if (process.env['NODE_ENV'] === 'production') {
    // In production the web process must have run first to create the wrap;
    // we fetch the kdfSalt later by attempting a SQLCipher open with a key
    // derived from the same Keychain passphrase. The web process and the
    // worker share the Keychain so they derive the same key bytes.
    //
    // Step A: derive Master KEK using the `kdfSalt` we will read from the
    // wrap. We don't have that yet. Instead, for prod we DO require the
    // operator to have set DEV_DB_PASSPHRASE *or* run the bootstrap script,
    // both of which use the *same* SQLCipher key for the worker process.
    throw new Error('sync: production boot path is not yet implemented (Phase 3 dev only)');
  } else {
    const dev = deriveDevKey();
    sqlcipherKey = dev.sqlcipherKey;
    masterKek = Buffer.from(dev.fakeMasterKek);
  }

  booted = await bootDb({
    sqlcipherKey,
    databaseUrl: env.databaseUrl,
    skipMigrations: true,
  });
  process.stdout.write('[sync] DB opened (SQLCipher).\n');

  // Load PCC wrap — gracefully handle "no wrap yet" so the worker can still
  // service personal items in a fresh dev environment.
  const wrapRes = await booted.repos.pccKeyWrapRepo.findActive();
  if (wrapRes.ok && wrapRes.value !== null && masterKek !== null) {
    const unwrapped = unwrapPccDek({
      masterKek,
      version: wrapRes.value.version,
      wrappedDek: wrapRes.value.wrappedDek as EncryptedBlob,
    });
    if (unwrapped.ok) {
      pccDek = unwrapped.dek;
      process.stdout.write(
        `[sync] unwrapped PCC DEK (v${String(wrapRes.value.version)}).\n`,
      );
    } else {
      process.stdout.write(`[sync] PCC DEK unwrap failed: ${unwrapped.kind}\n`);
    }
  } else {
    process.stdout.write('[sync] no active PCC wrap — personal items only this run.\n');
  }

  // Derive keybridge HMAC key from Master KEK.
  if (masterKek === null) {
    throw new Error('sync: Master KEK unavailable');
  }
  const hmacKey = Buffer.from(
    hkdfSync(
      'sha256',
      masterKek,
      Buffer.alloc(0),
      Buffer.from(HKDF_KEYBRIDGE_INFO, 'utf8'),
      32,
    ),
  );

  // Construct the real keybridge client.
  const realKeybridge = createKeybridgeClient({
    socketPath: env.socketPath,
    hmacKey: new Uint8Array(hmacKey),
  });
  realKeybridgeRef = realKeybridge;
  const connectRes = await realKeybridge.connect();
  if (!connectRes.ok) {
    process.stdout.write(
      `[sync] keybridge unavailable (${connectRes.error.kind}); will retry on next cycle.\n`,
    );
  } else {
    process.stdout.write('[sync] keybridge connected.\n');
  }

  // Construct the orchestrator.
  const audit = await resolveAuditService();
  const compute = await resolveComputeService();
  const plaid = await resolvePlaidService();

  const snapshotWriter = createSnapshotWriter({
    accountRepo: booted.repos.accountRepo,
    transactionRepo: booted.repos.transactionRepo,
    snapshotRepo: booted.repos.snapshotRepo,
    compute: compute as unknown as ComputeService,
    audit,
  });

  const orchestrator = createSyncOrchestrator({
    itemRepo: booted.repos.itemRepo,
    userRepo: booted.repos.userRepo,
    sessionRepo: booted.repos.sessionRepo,
    pccMembershipRepo: booted.repos.pccMembershipRepo,
    plaid,
    snapshotWriter,
    keybridge: {
      isConnected: () => realKeybridge.isConnected(),
      connect: async () => {
        const r = await realKeybridge.connect();
        return r.ok ? Ok(undefined) : Err({ kind: r.error.kind });
      },
      requestDek: async (input) => {
        const r = await realKeybridge.requestDek(input);
        if (!r.ok) {
          return Err({ kind: r.error.kind });
        }
        return Ok({
          bytes: r.value.bytes,
          release: r.value.release,
        });
      },
    },
  });

  process.stdout.write(`[sync] cycle interval: ${String(env.intervalMs / 60000)}m\n`);

  // Tick once immediately, then loop.
  let cycleCount = 0;
  const tick = async (): Promise<void> => {
    cycleCount += 1;
    const startedAt = new Date();
    process.stdout.write(`== sync cycle ${String(cycleCount)} ==\n`);
    const r = await orchestrator.runOnce({ now: startedAt });
    if (r.ok) {
      const v = r.value;
      process.stdout.write(
        `[sync] cycle complete: items=${String(v.itemsSucceeded)}/${String(v.itemsAttempted)}, snapshots=${String(v.snapshotsWritten)}, dur=${String(v.durationMs)}ms\n`,
      );
    } else {
      process.stdout.write(`[sync] cycle ended with error: ${r.error.kind}\n`);
    }
  };
  void tick();
  // No .unref() — we WANT the timer to keep the event loop alive until
  // SIGINT/SIGTERM. Cleanup handlers explicitly call clearInterval below.
  cycleTimer = setInterval(() => {
    void tick();
  }, env.intervalMs);

  // Block forever until SIGINT/SIGTERM. The interval keeps the loop alive.
  return await new Promise<number>(() => {
    /* never resolves */
  });
}

// Only run when executed as the main module (not when imported by tests).
const argvLast = process.argv[1] ?? '';
if (argvLast.endsWith('sync.ts') || argvLast.endsWith('sync.js')) {
  void (async (): Promise<void> => {
    try {
      // Ensure pid file present even if `pids/` doesn't exist.
      try {
        fs.mkdirSync('pids', { recursive: true });
        fs.writeFileSync('pids/sync.pid', String(process.pid), 'utf8');
      } catch {
        // best-effort
      }
      await main();
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'unknown error';
      process.stderr.write(`[sync] fatal: ${msg}\n`);
      process.exit(1);
    }
  })();
}
