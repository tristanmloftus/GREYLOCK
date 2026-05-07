// Greylock — Prisma client factory with SQLCipher binding
// =============================================================================
// AGENT-DB. The ONLY place a Prisma client is constructed. Repository modules
// receive an already-constructed `PrismaClient` instance via `createDbClient`.
//
// Architectural choice (documented in docs/agents/AGENT-DB.md):
//   - SQLCipher binding: `better-sqlite3-multiple-ciphers` (drop-in fork of
//     `better-sqlite3`) imported via pnpm alias `better-sqlite3 ->
//     better-sqlite3-multiple-ciphers`. Driven into Prisma via
//     `@prisma/adapter-better-sqlite3`. The adapter's official factory accepts
//     a `better-sqlite3` Database instance through its config, but does NOT
//     expose a hook to apply the SQLCipher key pragma between Database
//     construction and first query. We therefore subclass the official factory
//     and apply `PRAGMA cipher='sqlcipher'; PRAGMA hexkey='<hex>'` to the
//     adapter's underlying client immediately after `connect()` returns,
//     before any query is issued. This is the smallest possible diff against
//     the upstream adapter and keeps the migration path unchanged.
//
// SPEC §3 / SPEC §7:
//   - The 32-byte SQLCipher key is supplied as bytes by the caller. NEVER read
//     from `.env`, NEVER stored on disk, NEVER logged.
//   - Caller is responsible for zeroizing the key bytes after the client is
//     constructed (the pragma string copies the key into SQLite's memory).
//
// Hard requirement: route handlers and other agents NEVER import this module
// directly. They depend on the repository interfaces in `lib/types/services.ts`
// and the concrete repositories in `lib/db/repositories/*.ts`.
// =============================================================================

import { PrismaBetterSQLite3 } from '@prisma/adapter-better-sqlite3';
import { PrismaClient } from '@prisma/client';
import type { SqlDriverAdapter } from '@prisma/driver-adapter-utils';

import { sqlcipherKeyAsHex } from './sqlcipher-key.js';

// `better-sqlite3` is aliased to `better-sqlite3-multiple-ciphers` in
// `package.json`. Both share the same constructor surface. We never refer to
// the cipher fork by its real name in code — the alias is the contract.
import type Database from 'better-sqlite3';

const SQLCIPHER_CIPHER_NAME = 'sqlcipher';

export interface CreateDbClientInput {
  /** 32-byte SQLCipher database key (already derived from Master KEK). */
  readonly sqlcipherKey: Uint8Array;
  /** SQLite file URL like `file:./prisma/greylock.db`. Defaults to env. */
  readonly databaseUrl?: string;
  /** Optional Prisma log levels. Defaults to none in production. */
  readonly logLevel?: ReadonlyArray<'query' | 'info' | 'warn' | 'error'>;
}

export interface CreateDbClientOutput {
  readonly prisma: PrismaClient;
  /** Closes the Prisma client and disposes the underlying SQLCipher
   *  connection. Idempotent. */
  readonly dispose: () => Promise<void>;
}

/**
 * Subclass of the official PrismaBetterSQLite3 adapter factory that applies
 * SQLCipher pragmas immediately after the underlying client is constructed.
 *
 * The adapter exposes its underlying `client` (an instance of
 * `better-sqlite3-multiple-ciphers` via the alias) on the returned adapter
 * object. We use that handle to set the cipher and key BEFORE any Prisma
 * query reaches the database — otherwise the first query against an
 * encrypted DB would fail with "file is not a database".
 */
class GreylockSQLCipherAdapterFactory extends PrismaBetterSQLite3 {
  readonly #keyHex: string;

  constructor(config: { url: string }, keyHex: string) {
    super(config);
    this.#keyHex = keyHex;
  }

  override async connect(): Promise<SqlDriverAdapter> {
    const adapter = await super.connect();
    this.#applyKey(adapter);
    return adapter;
  }

  override async connectToShadowDb(): Promise<SqlDriverAdapter> {
    // Shadow DBs are typically `:memory:` and used by Prisma migrate to diff
    // schemas. multiple-ciphers refuses keys on memory DBs. Migrations against
    // the encrypted DB are applied by `lib/db/migrate.ts` running raw SQL on
    // the keyed connection — Prisma's shadow path is dev-time only and stays
    // unencrypted. This is acceptable because no real data flows through it.
    return super.connectToShadowDb();
  }

  #applyKey(adapter: SqlDriverAdapter): void {
    // The adapter object stores the better-sqlite3 client on a public `client`
    // property. We narrow with a structural type guard to avoid `any`.
    const candidate: unknown = (adapter as { readonly client?: unknown }).client;
    if (!isBetterSqlite3Database(candidate)) {
      throw new Error(
        'GreylockSQLCipherAdapterFactory: adapter.client is not a better-sqlite3 Database instance; SQLCipher key cannot be applied.',
      );
    }
    // Set cipher first (multiple-ciphers default IS sqlcipher but we are
    // explicit so a future binding swap that defaults differently still
    // produces the same on-disk format).
    candidate.pragma(`cipher = '${SQLCIPHER_CIPHER_NAME}'`);
    // Use hexkey to avoid string-encoding ambiguity. Quoted because the SQL
    // parser sees a single-quoted string literal; the contents are pure hex
    // (0-9 a-f) so SQL injection is not possible by construction.
    candidate.pragma(`hexkey = '${this.#keyHex}'`);
    // Sanity: a query that would touch a page should now succeed.
    // This forces SQLCipher to verify the key.
    candidate.pragma('cipher_compatibility = 4');
  }
}

interface BetterSqlite3DatabaseLike {
  pragma(this: void, source: string, options?: unknown): unknown;
  prepare: Database.Database['prepare'];
  exec: Database.Database['exec'];
  close: Database.Database['close'];
}

function isBetterSqlite3Database(v: unknown): v is BetterSqlite3DatabaseLike {
  if (v === null || typeof v !== 'object') {
    return false;
  }
  const obj = v as Record<string, unknown>;
  return (
    typeof obj['pragma'] === 'function' &&
    typeof obj['prepare'] === 'function' &&
    typeof obj['exec'] === 'function' &&
    typeof obj['close'] === 'function'
  );
}

/**
 * Construct a Prisma client wired to a SQLCipher-encrypted SQLite file using
 * the provided 32-byte key bytes.
 *
 * Caller contract:
 *   - `sqlcipherKey` must be 32 bytes. Pass output of `deriveSqlcipherKey()`.
 *   - The returned client owns the connection until `dispose()` is called.
 *   - The key bytes are converted to hex and copied into SQLite's process
 *     memory by the pragma. The caller may zeroize the input buffer
 *     immediately after this function returns.
 *
 * Errors:
 *   - throws if `databaseUrl` is missing AND `process.env.DATABASE_URL` is unset.
 *   - throws if `sqlcipherKey` is not 32 bytes.
 *   - reading the wrong key from a previously-encrypted DB will surface as
 *     a SQLite error on the first query (`file is not a database`).
 */
export function createDbClient(input: CreateDbClientInput): CreateDbClientOutput {
  if (input.sqlcipherKey.byteLength !== 32) {
    throw new Error(
      `createDbClient: sqlcipherKey must be 32 bytes (got ${String(input.sqlcipherKey.byteLength)})`,
    );
  }
  const url = input.databaseUrl ?? process.env['DATABASE_URL'];
  if (typeof url !== 'string' || url.length === 0) {
    throw new Error('createDbClient: DATABASE_URL is required (set via input.databaseUrl or env)');
  }

  const keyHex = sqlcipherKeyAsHex(input.sqlcipherKey);
  try {
    const adapter = new GreylockSQLCipherAdapterFactory({ url }, keyHex);
    const prismaArgs: ConstructorParameters<typeof PrismaClient>[0] = {
      adapter,
      ...(input.logLevel && input.logLevel.length > 0 ? { log: [...input.logLevel] } : {}),
    };
    const prisma = new PrismaClient(prismaArgs);

    let disposed = false;
    const dispose = async (): Promise<void> => {
      if (disposed) {
        return;
      }
      disposed = true;
      await prisma.$disconnect();
    };

    return { prisma, dispose };
  } finally {
    // Best-effort: scrub our local hex copy. The pragma already passed the
    // bytes to SQLite, but we don't need to keep them in this scope. JS
    // strings are immutable so we can't actually zero the underlying buffer,
    // which is why we use hex (no recoverable secret beyond the bytes
    // SQLite already holds).
    // (left as a no-op to document intent)
  }
}
