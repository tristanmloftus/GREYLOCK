// Greylock — audit module barrel
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). Public surface for the audit subsystem.
//
// Two factory shapes:
//   - `createAuditService()` (no args)  — used by the runtime
//      `services-registry`. Resolves the AuditRepository from the booted DB
//      singleton via `getBootedDb()` in `lib/db/index.ts`.
//   - `createAuditService(deps)`         — used by tests and any caller that
//      already has an `AuditRepository` in scope. Takes explicit deps.
//
// Either path returns an `AuditService` per `lib/types/services.ts`.
// =============================================================================

import type { AuditRepository } from '../db/repositories/audit.js';
import { getBootedDb } from '../db/index.js';
import type { AuditService } from '../types/services.js';

import { createAuditService as createAuditServiceWithDeps } from './service.js';

export { sanitizeDetails } from './sanitizer.js';
export type { SanitizerResult } from './sanitizer.js';
export { computeEntryHash, ZERO_PREV_HASH } from './chain.js';
export type { ComputeEntryHashInput } from './chain.js';

export interface CreateAuditServiceDeps {
  readonly auditRepo: AuditRepository;
}

/**
 * Overloaded factory:
 *   - With explicit deps: returns an `AuditService` wrapping the given
 *     repository. Tests use this.
 *   - With no args: resolves the repository from the booted DB singleton.
 *     The runtime `services-registry` expects this signature.
 */
export function createAuditService(deps: CreateAuditServiceDeps): AuditService;
export function createAuditService(): AuditService;
export function createAuditService(deps?: CreateAuditServiceDeps): AuditService {
  if (deps !== undefined) {
    return createAuditServiceWithDeps({ auditRepo: deps.auditRepo });
  }
  // No deps → resolve from the booted DB. `getBootedDb()` throws if the
  // singleton hasn't been registered (e.g. during a unit test running in
  // isolation). The services-registry catches that throw and surfaces 503.
  return createAuditServiceWithDeps({ auditRepo: getBootedDb().repos.auditRepo });
}
