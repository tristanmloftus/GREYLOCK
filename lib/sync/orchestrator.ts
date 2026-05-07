// Greylock — Sync orchestrator
// =============================================================================
// AGENT-SYNC (Phase 3). Implements `SyncOrchestrator.runOnce`. One pass:
//
//   1. PCC items: list active PCC items via the admin scope, request a PCC
//      DEK from the keybridge once per item, call `PlaidService.syncItem`,
//      write a snapshot.
//   2. Personal items: walk users with active sessions; for each userId,
//      request the per-user DEK, sync the user's personal items, write a
//      per-user snapshot. Skip users whose `requestDek` returns
//      `session_invalid`.
//
// `syncItem` (manual): used by `POST /api/sync/run` to trigger a one-off
// sync of a specific item the caller already has scope to view. The
// keybridge is bypassed for personal items because the web process
// already holds the user's DEK in memory.
// =============================================================================

import { Err, Ok } from '../types/domain.js';
import type {
  Item,
  ItemId,
  Result,
  UserId,
} from '../types/domain.js';
import type {
  ItemRepository,
  PccMembershipRepository,
  PlaidService,
  PlaidSyncResult,
  RepoScope,
  SessionRepository,
  SyncError,
  SyncOrchestrator,
  SyncOutcome,
  UserRepository,
} from '../types/services.js';

import type { createSnapshotWriter } from './snapshot-writer.js';

// We take the keybridge client interface from `lib/ipc/` indirectly to avoid
// a circular dependency. The interface used at runtime mirrors the public
// shape of `lib/ipc/keybridge-client.ts:createKeybridgeClient`.
export interface OrchestratorBorrowedDek {
  readonly bytes: Uint8Array;
  release(): Promise<void>;
}

export interface OrchestratorKeybridge {
  isConnected(): boolean;
  connect(): Promise<Result<void, { readonly kind: string }>>;
  requestDek(
    input: { kind: 'pcc' } | { userId: string; sessionId: string },
  ): Promise<Result<OrchestratorBorrowedDek, { readonly kind: string }>>;
}

// -----------------------------------------------------------------------------
// Public types
// -----------------------------------------------------------------------------

export interface SyncOrchestratorDeps {
  readonly itemRepo: ItemRepository;
  readonly userRepo: UserRepository;
  readonly sessionRepo: SessionRepository;
  readonly pccMembershipRepo: PccMembershipRepository;
  readonly plaid: PlaidService;
  readonly snapshotWriter: ReturnType<typeof createSnapshotWriter>;
  readonly keybridge: OrchestratorKeybridge;
  /** Test seam — defaults to `() => new Date()`. */
  readonly now?: () => Date;
}

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

export function createSyncOrchestrator(deps: SyncOrchestratorDeps): SyncOrchestrator {
  const now = deps.now ?? ((): Date => new Date());

  async function ensureKeybridge(): Promise<Result<void, SyncError>> {
    if (deps.keybridge.isConnected()) {
      return Ok(undefined);
    }
    const r = await deps.keybridge.connect();
    if (!r.ok) {
      return Err({ kind: 'keybridge_unavailable' });
    }
    return Ok(undefined);
  }

  async function runPccCycle(
    counters: { itemsAttempted: number; itemsSucceeded: number; itemsFailed: number; snapshotsWritten: number },
  ): Promise<void> {
    const adminScope: RepoScope = { kind: 'admin' };
    const list = await deps.itemRepo.list(adminScope, { domain: 'pcc' });
    if (!list.ok) {
      return;
    }
    const active = list.value.filter((i) => i.removedAt === null);
    if (active.length === 0) {
      return;
    }
    // Borrow PCC DEK once for the whole cycle.
    const dekRes = await deps.keybridge.requestDek({ kind: 'pcc' });
    if (!dekRes.ok) {
      counters.itemsAttempted += active.length;
      counters.itemsFailed += active.length;
      return;
    }
    try {
      for (const item of active) {
        counters.itemsAttempted += 1;
        const synced = await deps.plaid.syncItem({ itemId: item.id });
        if (!synced.ok) {
          counters.itemsFailed += 1;
          continue;
        }
        counters.itemsSucceeded += 1;
      }
      // One snapshot per cycle for PCC (domain-wide).
      const snap = await deps.snapshotWriter.writeSnapshot({
        scope: adminScope,
        domain: 'pcc',
        userId: null,
      });
      if (snap.ok) {
        counters.snapshotsWritten += 1;
      }
    } finally {
      await dekRes.value.release().catch(() => undefined);
    }
  }

  async function runPersonalCycle(
    counters: { itemsAttempted: number; itemsSucceeded: number; itemsFailed: number; snapshotsWritten: number },
  ): Promise<void> {
    // Walk all known users; for each, check session activity. We reach into
    // the user repository via `list()` if available; otherwise we walk the
    // active-session set (cheaper and avoids touching every user row).
    const usersRes = await deps.userRepo.list();
    if (!usersRes.ok) {
      return;
    }

    for (const u of usersRes.value) {
      const sess = await deps.sessionRepo.findActiveByUser(u.id);
      if (!sess.ok || sess.value === null) {
        continue;
      }
      const activeSession = sess.value;

      // Request the per-user DEK via the keybridge (which also re-validates
      // the active session server-side as a defense-in-depth check).
      const dekRes = await deps.keybridge.requestDek({
        userId: u.id as unknown as string,
        sessionId: activeSession.id as unknown as string,
      });
      if (!dekRes.ok) {
        // session_invalid / dek_unavailable / etc — skip this user this cycle.
        continue;
      }
      try {
        const itemsRes = await deps.itemRepo.list(
          { kind: 'personal', userId: u.id },
          { domain: 'personal' },
        );
        if (!itemsRes.ok) {
          continue;
        }
        const personalItems = itemsRes.value.filter((i) => i.removedAt === null);
        for (const item of personalItems) {
          counters.itemsAttempted += 1;
          const synced = await deps.plaid.syncItem({ itemId: item.id });
          if (!synced.ok) {
            counters.itemsFailed += 1;
            continue;
          }
          counters.itemsSucceeded += 1;
        }
        const snap = await deps.snapshotWriter.writeSnapshot({
          scope: { kind: 'personal', userId: u.id },
          domain: 'personal',
          userId: u.id,
        });
        if (snap.ok) {
          counters.snapshotsWritten += 1;
        }
      } finally {
        await dekRes.value.release().catch(() => undefined);
      }
    }
  }

  async function runOnce(input: { readonly now: Date }): Promise<Result<SyncOutcome, SyncError>> {
    void input;
    const startedAt = now();
    const ready = await ensureKeybridge();
    if (!ready.ok) {
      return Err(ready.error);
    }

    const counters = {
      itemsAttempted: 0,
      itemsSucceeded: 0,
      itemsFailed: 0,
      snapshotsWritten: 0,
    };

    await runPccCycle(counters);
    await runPersonalCycle(counters);

    const durationMs = now().getTime() - startedAt.getTime();
    const outcome: SyncOutcome = {
      ...counters,
      durationMs,
    };
    if (counters.itemsFailed > 0 && counters.itemsSucceeded === 0) {
      return Err({ kind: 'partial_failure', failures: counters.itemsFailed });
    }
    return Ok(outcome);
  }

  async function syncItem(input: {
    readonly itemId: ItemId;
    readonly initiatorUserId: UserId;
  }): Promise<Result<PlaidSyncResult, SyncError>> {
    // Locate the item under the initiator's scopes (personal first, PCC if
    // member). Out-of-scope returns `not_found` and we surface as 'unexpected'
    // — the route handler maps that to 404 to keep failure modes
    // indistinguishable.
    const personalScope: RepoScope = { kind: 'personal', userId: input.initiatorUserId };
    let item: Item | null = null;
    const personalLookup = await deps.itemRepo.findById(personalScope, input.itemId);
    if (personalLookup.ok && personalLookup.value !== null) {
      item = personalLookup.value;
    } else {
      const member = await deps.pccMembershipRepo.isActiveMember(input.initiatorUserId);
      if (member.ok && member.value) {
        const pccScope: RepoScope = { kind: 'pcc', memberOfUserId: input.initiatorUserId };
        const pccLookup = await deps.itemRepo.findById(pccScope, input.itemId);
        if (pccLookup.ok && pccLookup.value !== null) {
          item = pccLookup.value;
        }
      }
    }
    if (item === null) {
      return Err({ kind: 'unexpected', cause: 'item_not_found' });
    }

    const synced = await deps.plaid.syncItem({ itemId: item.id });
    if (!synced.ok) {
      return Err({ kind: 'unexpected', cause: synced.error.kind });
    }
    return Ok(synced.value);
  }

  return Object.freeze({
    runOnce,
    syncItem,
  });
}

