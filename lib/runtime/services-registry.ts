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
  AuditService,
  AuthService,
  CryptoService,
  PasskeyRepository,
  SessionRepository,
  UserRepository,
} from '../types/services.js';

import { createAuthService } from '../auth/index.js';
import type { RateLimitRepository } from '../auth/rate-limit.js';
import type { WrappedDekReader } from '../auth/wrapped-dek-reader.js';

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

/** Allow the runtime to inject mock services in tests. The test harness wipes
 *  this between test files. Production code never calls this. */
export interface RegistryOverrides {
  readonly crypto?: CryptoService;
  readonly repos?: ResolvedRepos;
  readonly audit?: AuditService;
}

let overrides: RegistryOverrides = {};

export function __setRegistryOverridesForTests(next: RegistryOverrides): void {
  overrides = next;
  cryptoSingleton = null;
  reposSingleton = null;
  auditSingleton = null;
  authSingleton = null;
}

export function __resetRegistryForTests(): void {
  overrides = {};
  cryptoSingleton = null;
  reposSingleton = null;
  auditSingleton = null;
  authSingleton = null;
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
