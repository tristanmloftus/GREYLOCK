// SQLCipher round-trip — proves the cipher is engaged.
// =============================================================================
// Per AGENT-DB brief tests:
//   - open with key K, write a row, close.
//   - reopen with K, read it back.
//   - reopen with WRONG key → fails. (This is the only way to be sure the
//     binding actually applied SQLCipher; a no-op key would round-trip
//     trivially.)
// =============================================================================

import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { bootDb, createDbClient } from '../../../lib/db/index.js';
import { deriveDevKey } from '../../../scripts/db/dev-key.js';

describe('SQLCipher round-trip', () => {
  let tempDir: string;
  let dbPath: string;
  let dbUrl: string;

  beforeEach(() => {
    tempDir = mkdtempSync(join(tmpdir(), 'greylock-sqlcipher-rt-'));
    dbPath = join(tempDir, 'greylock-test.db');
    dbUrl = `file:${dbPath}`;
    process.env['DEV_DB_PASSPHRASE'] = 'sqlcipher-roundtrip-test-passphrase';
  });

  afterEach(() => {
    try {
      rmSync(tempDir, { recursive: true, force: true });
    } catch {
      // ignore
    }
  });

  it('writes a row with key K and reads it back with the same key', async () => {
    const { sqlcipherKey } = deriveDevKey();
    const booted = await bootDb({ sqlcipherKey, databaseUrl: dbUrl });
    try {
      const u = await booted.repos.userRepo.create({
        email: 'a@example.test',
        displayName: 'Alice',
        role: 'member',
      });
      expect(u.ok).toBe(true);
    } finally {
      await booted.dispose();
    }

    // Reopen with same key.
    const booted2 = await bootDb({ sqlcipherKey, databaseUrl: dbUrl, skipMigrations: true });
    try {
      const found = await booted2.repos.userRepo.findByEmail('a@example.test');
      expect(found.ok).toBe(true);
      if (!found.ok) {throw new Error('unreachable');}
      expect(found.value).not.toBeNull();
      expect(found.value?.email).toBe('a@example.test');
    } finally {
      await booted2.dispose();
    }
  });

  it('fails to read when reopened with the WRONG key', async () => {
    const { sqlcipherKey } = deriveDevKey();
    const booted = await bootDb({ sqlcipherKey, databaseUrl: dbUrl });
    await booted.repos.userRepo.create({
      email: 'b@example.test',
      displayName: 'Bob',
      role: 'member',
    });
    await booted.dispose();

    // Wrong key (32 bytes, all zeros).
    const wrongKey = new Uint8Array(32);
    const wrongClient = createDbClient({ sqlcipherKey: wrongKey, databaseUrl: dbUrl });
    let threwForWrongKey = false;
    try {
      // Any query against an encrypted DB with wrong key fails because the
      // header decrypt fails. Our applyMigrations would also fail; we go
      // direct so the error surfaces unambiguously.
      await wrongClient.prisma.$queryRawUnsafe('SELECT 1');
    } catch {
      threwForWrongKey = true;
    } finally {
      await wrongClient.dispose();
    }
    expect(threwForWrongKey).toBe(true);
  });

  it('writes ciphertext to disk (no plaintext markers)', async () => {
    const { sqlcipherKey } = deriveDevKey();
    const booted = await bootDb({ sqlcipherKey, databaseUrl: dbUrl });
    const marker = 'CLEARTEXT_MARKER_DO_NOT_LEAK_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789';
    await booted.repos.userRepo.create({
      email: `${marker.toLowerCase()}@example.test`,
      displayName: marker,
      role: 'member',
    });
    await booted.dispose();

    const buf = readFileSync(dbPath);
    const idx = buf.toString('binary').indexOf(marker);
    expect(idx).toBe(-1);
  });

  it('refuses to derive a dev key with no DEV_DB_PASSPHRASE', () => {
    const prior = process.env['DEV_DB_PASSPHRASE'];
    delete process.env['DEV_DB_PASSPHRASE'];
    try {
      expect(() => deriveDevKey()).toThrow(/DEV_DB_PASSPHRASE/u);
    } finally {
      if (prior !== undefined) {
        process.env['DEV_DB_PASSPHRASE'] = prior;
      }
    }
  });

  it('refuses to derive a dev key in NODE_ENV=production', () => {
    // process.env's strict typing marks NODE_ENV readonly; mutate via a
    // cast so the test still exercises the production guard.
    const env = process.env as Record<string, string | undefined>;
    const prior = env['NODE_ENV'];
    env['NODE_ENV'] = 'production';
    try {
      expect(() => deriveDevKey()).toThrow(/production/iu);
    } finally {
      env['NODE_ENV'] = prior;
    }
  });
});
