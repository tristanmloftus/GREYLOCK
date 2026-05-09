// Greylock — service-locator registry
// =============================================================================
// AGENT-AUTH (Phase 2). The single place where route handlers resolve concrete
// service implementations. Every other module in `lib/auth/` and every route
// handler depends only on interfaces from `lib/types/services.ts`.
//
// Why a service locator (and not full DI plumbing): Phase 2 only — this gives
// route handlers a `getAuthService()` call that returns a fully-wired
// `AuthService` without each handler having to construct repos/crypto/audit
// itself. Phase 5 may refactor this into proper request-scoped DI; the
// indirection is intentionally cheap to remove.
//
// Coordination with AGENT-CRYPTO and AGENT-DB:
//   - Their concrete modules (`lib/crypto/*`, `lib/db/*`) may not exist yet at
//     typecheck time. We therefore use **dynamic `import()` with try/catch**
//     and lazy singletons. If a dependency module is missing, the call
//     surfaces a runtime error — but `pnpm typecheck` succeeds because no
//     static `import` is performed at module-load time.
//   - When Phase 3+ ships these modules, the dynamic imports resolve and the
//     same call sites work without changes.
// =============================================================================

import type {
  AccountRepository,
  AuditService,
  AuthService,
  CryptoService,
  ItemRepository,
  PasskeyRepository,
  PccMembershipRepository,
  PlaidService,
  SessionRepository,
  TransactionRepository,
  UserRepository,
} from '../types/services.js';
import type { PlaidApi } from 'plaid';

import { createAuthService } from '../auth/index.js';
import type { RateLimitRepository } from '../auth/rate-limit.js';
import type { WrappedDekReader } from '../auth/wrapped-dek-reader.js';
import type { PccKeyWrapRepository } from '../db/index.js';

// -----------------------------------------------------------------------------
// Repository / service bundles returned to callers
// -----------------------------------------------------------------------------

export interface ResolvedRepos {
  readonly userRepo: UserRepository;
  readonly passkeyRepo: PasskeyRepository;
  readonly sessionRepo: SessionRepository;
  readonly rateLimitRepo: RateLimitRepository;
  readonly wrappedDekReader: WrappedDekReader;
}

// -----------------------------------------------------------------------------
// Lazy singletons
// -----------------------------------------------------------------------------

let cryptoSingleton: CryptoService | null = null;
let reposSingleton: ResolvedRepos | null = null;
let auditSingleton: AuditService | null = null;
let authSingleton: AuthService | null = null;
let plaidSingleton: PlaidService | null = null;

/** Allow the runtime to inject mock services in tests. The test harness wipes
 *  this between test files. Production code never calls this. */
export interface RegistryOverrides {
  readonly crypto?: CryptoService;
  readonly repos?: ResolvedRepos;
  readonly audit?: AuditService;
  readonly plaid?: PlaidService;
}

let overrides: RegistryOverrides = {};

/**
 * Production guard for the test-only override hooks. Refuses to mutate the
 * registry unless we're explicitly in a test environment. This closes
 * QA-SEC Phase-2 §M-3: a malicious dependency reaching this surface in
 * production cannot swap the CryptoService.
 */
function assertTestEnv(caller: string): void {
  const isTestEnv =
    process.env['NODE_ENV'] === 'test' ||
    process.env['VITEST'] === 'true' ||
    process.env['VITEST'] === '1' ||
    process.env['GREYLOCK_TEST_MODE'] === '1';
  if (!isTestEnv) {
    throw new Error(
      `${caller}: registry overrides are forbidden outside the test environment ` +
        `(set NODE_ENV=test or GREYLOCK_TEST_MODE=1).`,
    );
  }
}

export function __setRegistryOverridesForTests(next: RegistryOverrides): void {
  assertTestEnv('__setRegistryOverridesForTests');
  overrides = next;
  cryptoSingleton = null;
  reposSingleton = null;
  auditSingleton = null;
  authSingleton = null;
  plaidSingleton = null;
}

export function __resetRegistryForTests(): void {
  assertTestEnv('__resetRegistryForTests');
  overrides = {};
  cryptoSingleton = null;
  reposSingleton = null;
  auditSingleton = null;
  authSingleton = null;
  plaidSingleton = null;
}

// -----------------------------------------------------------------------------
// Resolvers
// -----------------------------------------------------------------------------

/**
 * Resolve the CryptoService. Defers loading of `lib/crypto/*` to runtime so
 * `pnpm typecheck` can succeed before AGENT-CRYPTO's module exists.
 *
 * If AGENT-CRYPTO has not yet published `lib/crypto/index.ts`, this throws
 * `Error('CryptoService unavailable: ...')`. Routes catching this error MUST
 * surface 503 with a sanitized message.
 */
export async function getCryptoServiceLazy(): Promise<CryptoService> {
  if (overrides.crypto !== undefined) {
    return overrides.crypto;
  }
  if (cryptoSingleton !== null) {
    return cryptoSingleton;
  }
  let mod: { readonly createCryptoService?: (...args: unknown[]) => CryptoService } | null = null;
  try {
    // The variable indirection prevents `tsc` from statically resolving the
    // path so this file typechecks before AGENT-CRYPTO ships its module. The
    // `/* @vite-ignore */` hint mirrors that intent for any bundler tooling.
    const path = '../crypto/index.js';
    mod = (await import(/* @vite-ignore */ path)) as {
      readonly createCryptoService?: (...args: unknown[]) => CryptoService;
    };
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`CryptoService unavailable: ${message}`);
  }
  if (mod === null || typeof mod.createCryptoService !== 'function') {
    throw new Error('CryptoService unavailable: lib/crypto/index.ts missing createCryptoService');
  }
  cryptoSingleton = mod.createCryptoService();
  return cryptoSingleton;
}

/**
 * Resolve the repository bundle. Same pattern as `getCryptoServiceLazy`.
 */
export async function getRepos(): Promise<ResolvedRepos> {
  if (overrides.repos !== undefined) {
    return overrides.repos;
  }
  if (reposSingleton !== null) {
    return reposSingleton;
  }
  let mod: { readonly createRepositories?: () => ResolvedRepos } | null = null;
  try {
    const path = '../db/index.js';
    mod = (await import(/* @vite-ignore */ path)) as {
      readonly createRepositories?: () => ResolvedRepos;
    };
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`Repositories unavailable: ${message}`);
  }
  if (mod === null || typeof mod.createRepositories !== 'function') {
    throw new Error('Repositories unavailable: lib/db/index.ts missing createRepositories');
  }
  reposSingleton = mod.createRepositories();
  return reposSingleton;
}

/** Resolve the audit service. Mirrors the pattern above. */
export async function getAuditService(): Promise<AuditService> {
  if (overrides.audit !== undefined) {
    return overrides.audit;
  }
  if (auditSingleton !== null) {
    return auditSingleton;
  }
  let mod: { readonly createAuditService?: () => AuditService } | null = null;
  try {
    const path = '../audit/index.js';
    mod = (await import(/* @vite-ignore */ path)) as {
      readonly createAuditService?: () => AuditService;
    };
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`AuditService unavailable: ${message}`);
  }
  if (mod === null || typeof mod.createAuditService !== 'function') {
    throw new Error('AuditService unavailable: lib/audit/index.ts missing createAuditService');
  }
  auditSingleton = mod.createAuditService();
  return auditSingleton;
}

/** Resolve a fully-wired AuthService. */
export async function getAuthService(): Promise<AuthService> {
  if (authSingleton !== null) {
    return authSingleton;
  }
  const [cryptoSvc, repos, audit] = await Promise.all([
    getCryptoServiceLazy(),
    getRepos(),
    getAuditService(),
  ]);
  authSingleton = createAuthService({
    crypto: cryptoSvc,
    userRepo: repos.userRepo,
    passkeyRepo: repos.passkeyRepo,
    sessionRepo: repos.sessionRepo,
    wrappedDekReader: repos.wrappedDekReader,
    audit,
  });
  return authSingleton;
}

/**
 * Resolve a fully-wired `PlaidService`. Same lazy-singleton pattern as the
 * other services. AGENT-PLAID added this accessor in Phase 3 — see retro at
 * `docs/agents/AGENT-PLAID.md`. The route handlers under `app/api/plaid/*`
 * and AGENT-SYNC's orchestrator both consume this.
 *
 * The Plaid bundle requires the full repository set (including item /
 * account / transaction / pcc-membership / pcc-key-wrap) which is not part
 * of `ResolvedRepos`. We fetch it separately via `lib/db/index.js#getBootedDb`
 * to avoid bloating the registry-wide bundle. If the booted DB hasn't been
 * registered yet, this surfaces a 503 in the route layer.
 */
export async function getPlaidService(): Promise<PlaidService> {
  if (overrides.plaid !== undefined) {
    return overrides.plaid;
  }
  if (plaidSingleton !== null) {
    return plaidSingleton;
  }
  const [cryptoSvc, audit] = await Promise.all([getCryptoServiceLazy(), getAuditService()]);

  // Fetch the full booted-DB bundle (includes Item/Account/Transaction repos).
  type DbModShape = {
    readonly getBootedDb?: () => {
      readonly repos: {
        readonly itemRepo: ItemRepository;
        readonly accountRepo: AccountRepository;
        readonly transactionRepo: TransactionRepository;
        readonly userRepo: UserRepository;
        readonly pccMembershipRepo: PccMembershipRepository;
        readonly pccKeyWrapRepo: PccKeyWrapRepository;
      };
    };
  };
  let dbMod: DbModShape | null = null;
  try {
    const dbPath = '../db/index.js';
    dbMod = (await import(/* @vite-ignore */ dbPath)) as DbModShape;
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`PlaidService unavailable: ${message}`);
  }
  if (dbMod === null || typeof dbMod.getBootedDb !== 'function') {
    throw new Error('PlaidService unavailable: lib/db/index.ts missing getBootedDb');
  }
  const repos = dbMod.getBootedDb().repos;

  // Wire the Plaid SDK client + service.
  type PlaidModShape = {
    readonly createPlaidServiceWithBroker?: (input: {
      readonly plaidClient: PlaidApi;
      readonly crypto: CryptoService;
      readonly itemRepo: ItemRepository;
      readonly accountRepo: AccountRepository;
      readonly transactionRepo: TransactionRepository;
      readonly userRepo: UserRepository;
      readonly pccMembershipRepo: PccMembershipRepository;
      readonly pccKeyWrapRepo: PccKeyWrapRepository;
      readonly audit: AuditService;
      readonly clientName: string;
      readonly countryCodes: ReadonlyArray<string>;
      readonly defaultProducts: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
    }) => PlaidService;
    readonly getPlaidClient?: () => PlaidApi;
    readonly getPlaidConfig?: () => {
      readonly products: ReadonlyArray<'transactions' | 'auth' | 'identity'>;
      readonly countryCodes: ReadonlyArray<string>;
      readonly environment: 'sandbox' | 'development' | 'production';
    };
  };
  let plaidMod: PlaidModShape | null = null;
  try {
    const plaidPath = '../plaid/index.js';
    plaidMod = (await import(/* @vite-ignore */ plaidPath)) as PlaidModShape;
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`PlaidService unavailable: ${message}`);
  }
  if (
    plaidMod === null ||
    typeof plaidMod.createPlaidServiceWithBroker !== 'function' ||
    typeof plaidMod.getPlaidClient !== 'function' ||
    typeof plaidMod.getPlaidConfig !== 'function'
  ) {
    throw new Error('PlaidService unavailable: lib/plaid/index.ts missing factory exports');
  }
  const plaidClient = plaidMod.getPlaidClient();
  const cfg = plaidMod.getPlaidConfig();
  const created = plaidMod.createPlaidServiceWithBroker({
    plaidClient,
    crypto: cryptoSvc,
    itemRepo: repos.itemRepo,
    accountRepo: repos.accountRepo,
    transactionRepo: repos.transactionRepo,
    userRepo: repos.userRepo,
    pccMembershipRepo: repos.pccMembershipRepo,
    pccKeyWrapRepo: repos.pccKeyWrapRepo,
    audit,
    clientName: 'Greylock',
    countryCodes: cfg.countryCodes,
    defaultProducts: cfg.products,
  });
  plaidSingleton = created;
  return created;
}

/**
 * Test accessor: same shape as `getRepos` but returns the full booted-DB
 * repository bundle that the Plaid service requires. Tests that exercise the
 * Plaid layer pull repos via this helper.
 */
export async function getFullRepos(): Promise<{
  readonly itemRepo: ItemRepository;
  readonly accountRepo: AccountRepository;
  readonly transactionRepo: TransactionRepository;
  readonly userRepo: UserRepository;
  readonly pccMembershipRepo: PccMembershipRepository;
  readonly pccKeyWrapRepo: PccKeyWrapRepository;
}> {
  type DbModShape = {
    readonly getBootedDb?: () => {
      readonly repos: {
        readonly itemRepo: ItemRepository;
        readonly accountRepo: AccountRepository;
        readonly transactionRepo: TransactionRepository;
        readonly userRepo: UserRepository;
        readonly pccMembershipRepo: PccMembershipRepository;
        readonly pccKeyWrapRepo: PccKeyWrapRepository;
      };
    };
  };
  let dbMod: DbModShape | null = null;
  try {
    const dbPath = '../db/index.js';
    dbMod = (await import(/* @vite-ignore */ dbPath)) as DbModShape;
  } catch (cause: unknown) {
    const message = cause instanceof Error ? cause.message : 'unknown';
    throw new Error(`Repositories unavailable: ${message}`);
  }
  if (dbMod === null || typeof dbMod.getBootedDb !== 'function') {
    throw new Error('Repositories unavailable: lib/db/index.ts missing getBootedDb');
  }
  return dbMod.getBootedDb().repos;
}
