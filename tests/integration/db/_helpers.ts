// Greylock — db integration test helpers
// =============================================================================
// Spins up a fresh SQLCipher-encrypted DB per test, applies the canonical
// migration, and returns repos + Prisma client. Exports a `withFreshDb()`
// helper for the per-test pattern.
// =============================================================================

import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { bootDb, type AllRepositories, type BootedDb } from '../../../lib/db/index.js';
import { deriveDevKey } from '../../../scripts/db/dev-key.js';

export interface TestDb {
  readonly booted: BootedDb;
  readonly repos: AllRepositories;
  readonly tempDir: string;
  readonly dbPath: string;
  readonly cleanup: () => Promise<void>;
}

export async function makeTestDb(opts?: { passphrase?: string }): Promise<TestDb> {
  const tempDir = mkdtempSync(join(tmpdir(), 'greylock-db-test-'));
  const dbPath = join(tempDir, 'greylock-test.db');
  const dbUrl = `file:${dbPath}`;

  process.env['DEV_DB_PASSPHRASE'] = opts?.passphrase ?? 'test-passphrase-do-not-use-in-production';
  const { sqlcipherKey } = deriveDevKey();

  const booted = await bootDb({
    sqlcipherKey,
    databaseUrl: dbUrl,
    migrationsDir: 'prisma/migrations',
  });

  return {
    booted,
    repos: booted.repos,
    tempDir,
    dbPath,
    cleanup: async () => {
      await booted.dispose();
      try {
        rmSync(tempDir, { recursive: true, force: true });
      } catch {
        // best-effort
      }
    },
  };
}
