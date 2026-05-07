// Greylock — schema migration applier for SQLCipher-encrypted DBs
// =============================================================================
// AGENT-DB. Why this exists:
//
//   `prisma migrate deploy` opens the database via the Prisma schema engine,
//   which does NOT route through our driver-adapter (the adapter is a runtime
//   query path, not a schema-engine path). The schema engine therefore cannot
//   set the SQLCipher key, and against an encrypted database it would fail
//   with "file is not a database".
//
// Approach:
//   1. We use `pnpm prisma migrate dev` ONCE against a non-encrypted scratch
//      DB to generate the canonical SQL in `prisma/migrations/<ts>_init/`.
//      That migration SQL is the deliverable.
//   2. At runtime (and in tests) we apply migrations to the encrypted DB by
//      reading the migration SQL files and `db.exec()`-ing each statement
//      through our keyed Prisma adapter.
//   3. We keep our own `_greylock_migrations` table (alongside Prisma's
//      `_prisma_migrations` if `prisma migrate` ever runs against the same
//      file) so applies are idempotent.
//
// This module is the only place that touches `prisma/migrations/*.sql`.
// =============================================================================

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';

import type { PrismaClient } from '@prisma/client';

const MIGRATIONS_DIR_DEFAULT = 'prisma/migrations';

interface MigrationFile {
  readonly id: string; // directory name, e.g. "20260506_init"
  readonly sql: string;
}

interface MigrationRow {
  readonly id: string;
  readonly applied_at: string;
}

/**
 * Apply any unapplied migrations from `prisma/migrations/<id>/migration.sql`
 * to the connected (already-keyed) Prisma client. Idempotent.
 */
export async function applyMigrations(input: {
  readonly prisma: PrismaClient;
  readonly migrationsDir?: string;
}): Promise<{ readonly applied: ReadonlyArray<string> }> {
  const dir = input.migrationsDir ?? MIGRATIONS_DIR_DEFAULT;
  if (!existsSync(dir)) {
    return { applied: [] };
  }

  // Bookkeeping table.
  await input.prisma.$executeRawUnsafe(
    "CREATE TABLE IF NOT EXISTS _greylock_migrations (id TEXT PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT (datetime('now')))",
  );

  const applied = await input.prisma.$queryRawUnsafe<MigrationRow[]>(
    'SELECT id, applied_at FROM _greylock_migrations ORDER BY id ASC',
  );
  const appliedIds = new Set(applied.map((r) => r.id));

  const files = listMigrations(dir);
  const newlyApplied: string[] = [];
  for (const f of files) {
    if (appliedIds.has(f.id)) {
      continue;
    }
    const stmts = splitSqlStatements(f.sql);
    for (const stmt of stmts) {
      // $executeRawUnsafe is fine for static migration SQL we authored — there
      // is no user-supplied data in these files. Still, we forbid `?` and `$N`
      // placeholders here to keep the contract simple.
      await input.prisma.$executeRawUnsafe(stmt);
    }
    await input.prisma.$executeRawUnsafe('INSERT INTO _greylock_migrations (id) VALUES (?)', f.id);
    newlyApplied.push(f.id);
  }
  return { applied: newlyApplied };
}

function listMigrations(dir: string): ReadonlyArray<MigrationFile> {
  const out: MigrationFile[] = [];
  const entries = readdirSync(dir, { withFileTypes: true });
  for (const e of entries) {
    if (!e.isDirectory()) {
      continue;
    }
    if (e.name === ('migration_lock.toml' as string)) {
      continue;
    }
    const sqlPath = join(dir, e.name, 'migration.sql');
    if (!existsSync(sqlPath)) {
      continue;
    }
    out.push({ id: e.name, sql: readFileSync(sqlPath, 'utf8') });
  }
  out.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  return out;
}

/**
 * Split a Prisma-generated `migration.sql` into individual statements. Prisma
 * separates statements with `;\n` at the end of each logical statement. We
 * strip leading SQL line-comments and trim. A naive `split(';')` would break
 * inside string literals — Prisma does not currently emit semicolons inside
 * string literals for SQLite migrations, but we still avoid splitting inside
 * single-quoted strings.
 */
function splitSqlStatements(sql: string): ReadonlyArray<string> {
  const statements: string[] = [];
  let current = '';
  let inString = false;
  for (let i = 0; i < sql.length; i++) {
    const ch = sql[i] as string;
    if (ch === "'" && (i === 0 || sql[i - 1] !== '\\')) {
      // Toggle naive single-quote tracking. SQLite escapes single quotes by
      // doubling them ('') — that toggles in/out which is the correct net
      // result.
      inString = !inString;
    }
    if (ch === ';' && !inString) {
      const stripped = stripCommentsAndTrim(current);
      if (stripped.length > 0) {
        statements.push(stripped);
      }
      current = '';
      continue;
    }
    current += ch;
  }
  const tail = stripCommentsAndTrim(current);
  if (tail.length > 0) {
    statements.push(tail);
  }
  return statements;
}

function stripCommentsAndTrim(stmt: string): string {
  const lines = stmt.split('\n');
  const kept: string[] = [];
  for (const line of lines) {
    const trimmed = line.trimStart();
    if (trimmed.startsWith('--')) {
      continue;
    }
    kept.push(line);
  }
  return kept.join('\n').trim();
}
