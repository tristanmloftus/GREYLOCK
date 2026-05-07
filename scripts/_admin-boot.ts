// Greylock — shared admin-script boot helper
// =============================================================================
// Orchestrator (Phase 3). Dev-only boot for `pnpm admin:*` scripts. Mirrors
// the pattern in `scripts/sync.ts`: derive SQLCipher key + Master KEK from
// DEV_DB_PASSPHRASE, open the DB, build repos. Production boot path is
// Phase 5 work.
//
// Every admin invocation gets:
//   - a booted DB (Prisma + repos)
//   - a Master KEK Buffer (zeroized in `dispose`)
//   - a `dispose()` callback that always cleans up
//
// Hard guard: refuses to run with NODE_ENV=production. The production path
// requires Keychain integration via `lib/crypto/master-key.ts:withPassphraseBytes`
// and is intentionally not stubbed here — operators must run real production
// boot through the web process.
// =============================================================================

import { Buffer } from 'node:buffer';

import type { BootedDb } from '../lib/db/index.js';
import { bootDb, registerBootedDbSingleton } from '../lib/db/index.js';

import { deriveDevKey } from './db/dev-key.js';

export interface AdminBoot {
  readonly booted: BootedDb;
  readonly masterKek: Buffer;
  readonly dispose: () => Promise<void>;
}

export async function bootForAdmin(): Promise<AdminBoot> {
  if (process.env['NODE_ENV'] === 'production') {
    throw new Error(
      'admin scripts: production boot path is not yet wired (Phase 5). ' +
        'Run admin actions through the web app /api/admin/* routes instead.',
    );
  }
  const databaseUrl = process.env['DATABASE_URL'];
  if (databaseUrl === undefined || databaseUrl.length === 0) {
    throw new Error('admin: DATABASE_URL is required');
  }
  const dev = deriveDevKey();
  const masterKek = Buffer.from(dev.fakeMasterKek);
  const booted = await bootDb({
    sqlcipherKey: dev.sqlcipherKey,
    databaseUrl,
    skipMigrations: true,
  });
  registerBootedDbSingleton(booted);

  let disposed = false;
  const dispose = async (): Promise<void> => {
    if (disposed) {
      return;
    }
    disposed = true;
    masterKek.fill(0);
    try {
      await booted.dispose();
    } catch {
      // best-effort
    }
  };
  return { booted, masterKek, dispose };
}

/** Parse a positional CLI arg or fail with a usage message. */
export function requireArg(idx: number, _name: string, usage: string): string {
  const v = process.argv[idx];
  if (v === undefined || v.length === 0) {
    process.stderr.write(`${usage}\n`);
    process.exit(2);
  }
  return v;
}

/** Look up a `--flag value` style arg. Returns undefined if missing. */
export function findFlag(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  if (i === -1 || i + 1 >= process.argv.length) {
    return undefined;
  }
  return process.argv[i + 1];
}

/** Run `fn`, dispose, and exit with appropriate status. Errors print to stderr. */
export async function runAdmin(fn: (boot: AdminBoot) => Promise<number>): Promise<void> {
  let boot: AdminBoot | null = null;
  let exitCode = 1;
  try {
    boot = await bootForAdmin();
    exitCode = await fn(boot);
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    process.stderr.write(`admin: ${msg}\n`);
    exitCode = 1;
  } finally {
    if (boot !== null) {
      await boot.dispose();
    }
  }
  process.exit(exitCode);
}
