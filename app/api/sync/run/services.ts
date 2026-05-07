// Greylock — sync route service resolver
// =============================================================================
// AGENT-SYNC (Phase 3). Tiny indirection so the route handler can resolve a
// fully-wired SyncOrchestrator without statically importing concrete modules
// that may not exist at type-check time. Mirrors the dynamic-import pattern
// used by `lib/runtime/services-registry.ts`.
//
// In production, `lib/runtime/boot.ts` calls `__setSyncOrchestratorForTests`
// with the real instance once the keybridge server is up. Tests inject mocks
// the same way.
// =============================================================================

import type { SyncOrchestrator } from '../../../../lib/types/services.js';

let override: SyncOrchestrator | null = null;

export function __setSyncOrchestratorForTests(svc: SyncOrchestrator | null): void {
  override = svc;
}

export async function getSyncOrchestrator(): Promise<SyncOrchestrator> {
  if (override !== null) {
    return override;
  }
  // No production resolution path yet — `pnpm sync` owns the orchestrator
  // singleton. The web process side will be wired in `lib/runtime/boot.ts`
  // (out of scope for this file). For now, surface a 503 by throwing so the
  // route returns service_unavailable.
  throw new Error('SyncOrchestrator not registered; web process must wire via __setSyncOrchestratorForTests at boot');
}
