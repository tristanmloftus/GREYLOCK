// Greylock — db barrel
// =============================================================================
// AGENT-DB. The single import surface other agents see. Route handlers and
// services NEVER import from `lib/db/repositories/*` directly; they go
// through this file.
//
// Lifecycle:
//   1. AGENT-CRYPTO loads the Master KEK at boot.
//   2. AGENT-CRYPTO derives the SQLCipher key via `deriveSqlcipherKey()`.
//   3. AGENT-CRYPTO calls `bootDb({ sqlcipherKey })`. This wires up Prisma,
//      applies any pending migrations, and constructs every repository.
//   4. The lazy `createRepositories()` returns the repos for the registry.
//
// Test wiring: tests call `bootDb()` directly (or `bootDbForTests()`) and
// receive both the Prisma client and the repos.
// =============================================================================

import type { PrismaClient } from '@prisma/client';

import type {
  AccountRepository,
  ItemRepository,
  PasskeyRepository,
  PccMembershipRepository,
  RepoScope,
  SessionRepository,
  SnapshotRepository,
  TransactionRepository,
  UserRepository,
} from '../types/services.js';
import type { RateLimitRepository } from '../auth/rate-limit.js';
import type { EnrollmentTokenRepository } from '../auth/enrollment-token.js';

import { createDbClient } from './client.js';
import { applyMigrations } from './migrate.js';
import { createAuditRepository, type AuditRepository } from './repositories/audit.js';
import { createAccountRepository } from './repositories/account.js';
import { createEnrollmentTokenRepository } from './repositories/enrollment-token.js';
import { createItemRepository } from './repositories/item.js';
import { createPasskeyRepository } from './repositories/passkey.js';
import {
  createPccKeyWrapRepository,
  type PccKeyWrapRepository,
} from './repositories/pcc-key-wrap.js';
import { createPccMembershipRepository } from './repositories/pcc-membership.js';
import { createRateLimitRepository } from './repositories/rate-limit.js';
import { createSessionRepository } from './repositories/session.js';
import { createSnapshotRepository } from './repositories/snapshot.js';
import { createTransactionRepository } from './repositories/transaction.js';
import { createUserRepository } from './repositories/user.js';

// -----------------------------------------------------------------------------
// Public types re-exported for cross-agent consumers.
// -----------------------------------------------------------------------------

export type { AuditRepository } from './repositories/audit.js';
export type { PccKeyWrapRecord, PccKeyWrapRepository } from './repositories/pcc-key-wrap.js';
export { createDbClient, type CreateDbClientInput, type CreateDbClientOutput } from './client.js';
export { deriveSqlcipherKey, sqlcipherKeyAsHex, SQLCIPHER_KEY_HKDF_INFO } from './sqlcipher-key.js';
export { mintEnrollmentToken } from './repositories/enrollment-token.js';
export { applyMigrations } from './migrate.js';
export { computeEntryHash } from './repositories/audit.js';

// -----------------------------------------------------------------------------
// Repository bundle — what AGENT-AUTH's services-registry consumes.
// -----------------------------------------------------------------------------

export interface AllRepositories {
  readonly userRepo: UserRepository;
  readonly passkeyRepo: PasskeyRepository;
  readonly sessionRepo: SessionRepository;
  readonly itemRepo: ItemRepository;
  readonly accountRepo: AccountRepository;
  readonly transactionRepo: TransactionRepository;
  readonly snapshotRepo: SnapshotRepository;
  readonly pccMembershipRepo: PccMembershipRepository;
  readonly pccKeyWrapRepo: PccKeyWrapRepository;
  readonly rateLimitRepo: RateLimitRepository;
  readonly auditRepo: AuditRepository;
  readonly enrollmentTokenRepo: EnrollmentTokenRepository;
}

/**
 * Wire every repository against a single shared Prisma client. The client
 * MUST already be SQLCipher-keyed (see `createDbClient`). Repositories are
 * stateless except for their captured `prisma` reference; multiple calls
 * with the same client return distinct repository instances safely.
 */
export function buildRepositories(prisma: PrismaClient): AllRepositories {
  return {
    userRepo: createUserRepository({ prisma }),
    passkeyRepo: createPasskeyRepository({ prisma }),
    sessionRepo: createSessionRepository({ prisma }),
    itemRepo: createItemRepository({ prisma }),
    accountRepo: createAccountRepository({ prisma }),
    transactionRepo: createTransactionRepository({ prisma }),
    snapshotRepo: createSnapshotRepository({ prisma }),
    pccMembershipRepo: createPccMembershipRepository({ prisma }),
    pccKeyWrapRepo: createPccKeyWrapRepository({ prisma }),
    rateLimitRepo: createRateLimitRepository({ prisma }),
    auditRepo: createAuditRepository({ prisma }),
    enrollmentTokenRepo: createEnrollmentTokenRepository({ prisma }),
  };
}

// -----------------------------------------------------------------------------
// Boot helpers
// -----------------------------------------------------------------------------

export interface BootDbInput {
  /** 32-byte SQLCipher key. Caller derived from Master KEK. */
  readonly sqlcipherKey: Uint8Array;
  /** Override of DATABASE_URL (test wiring). */
  readonly databaseUrl?: string;
  /** If true, skip applying migrations (tests that pre-seed the DB). */
  readonly skipMigrations?: boolean;
  /** Override of the migrations directory (tests). */
  readonly migrationsDir?: string;
}

export interface BootedDb {
  readonly prisma: PrismaClient;
  readonly repos: AllRepositories;
  readonly dispose: () => Promise<void>;
}

/**
 * Construct a Prisma client wired to a SQLCipher-encrypted SQLite file,
 * apply migrations, and build every repository. The returned `dispose()`
 * tears down the connection idempotently.
 */
export async function bootDb(input: BootDbInput): Promise<BootedDb> {
  const created = createDbClient({
    sqlcipherKey: input.sqlcipherKey,
    ...(input.databaseUrl !== undefined ? { databaseUrl: input.databaseUrl } : {}),
  });
  if (input.skipMigrations !== true) {
    await applyMigrations({
      prisma: created.prisma,
      ...(input.migrationsDir !== undefined ? { migrationsDir: input.migrationsDir } : {}),
    });
  }
  return {
    prisma: created.prisma,
    repos: buildRepositories(created.prisma),
    dispose: created.dispose,
  };
}

// -----------------------------------------------------------------------------
// Singleton wiring for the services-registry.
// -----------------------------------------------------------------------------
//
// AGENT-AUTH's `lib/runtime/services-registry.ts` calls a dynamic `import()`
// of this module and expects a `createRepositories()` factory + a
// pre-resolved `enrollmentTokenRepo` (it imports the latter directly to
// avoid a Promise hop in the registration-begin route hot path).
//
// To stay consistent with the registry contract, we expose a singleton
// stash that AGENT-CRYPTO populates at boot. If the singleton hasn't been
// populated, `createRepositories()` throws — the route handler then surfaces
// 503 (the registry's own "service unavailable" path).
// -----------------------------------------------------------------------------

let bootedSingleton: BootedDb | null = null;

/** Called by AGENT-CRYPTO from `lib/runtime/boot.ts` after Master KEK load. */
export function registerBootedDbSingleton(b: BootedDb): void {
  bootedSingleton = b;
}

/** Returns the booted DB if already registered; throws otherwise. */
export function getBootedDb(): BootedDb {
  if (bootedSingleton === null) {
    throw new Error('lib/db: bootedSingleton not registered (call bootDb() at startup)');
  }
  return bootedSingleton;
}

/** Returns just the repository bundle for the services-registry. */
export interface RegistryRepoBundle {
  readonly userRepo: UserRepository;
  readonly passkeyRepo: PasskeyRepository;
  readonly sessionRepo: SessionRepository;
  readonly rateLimitRepo: RateLimitRepository;
}

/** Adapter for `lib/runtime/services-registry.ts`'s expected shape. */
export function createRepositories(): RegistryRepoBundle {
  const b = getBootedDb();
  return {
    userRepo: b.repos.userRepo,
    passkeyRepo: b.repos.passkeyRepo,
    sessionRepo: b.repos.sessionRepo,
    rateLimitRepo: b.repos.rateLimitRepo,
  };
}

/** Direct accessor for the EnrollmentTokenRepository — imported by the
 *  registration-begin route through `mod.enrollmentTokenRepo`. */
export const enrollmentTokenRepo: {
  verify: EnrollmentTokenRepository['verify'];
  consume: EnrollmentTokenRepository['consume'];
} = {
  verify(input) {
    return getBootedDb().repos.enrollmentTokenRepo.verify(input);
  },
  consume(input) {
    return getBootedDb().repos.enrollmentTokenRepo.consume(input);
  },
};

// Re-export the scope shape for other modules.
export type { RepoScope };
