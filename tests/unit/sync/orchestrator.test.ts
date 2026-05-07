// Tests for `lib/sync/orchestrator.ts` with all I/O mocked.

import { describe, it, expect } from 'vitest';

import { createSyncOrchestrator } from '../../../lib/sync/orchestrator.js';
import { Err, Ok } from '../../../lib/types/domain.js';
import { ItemId, SessionId, UserId } from '../../../lib/types/domain.js';
import type {
  Item,
  Session,
  User,
} from '../../../lib/types/domain.js';
import type {
  ItemRepository,
  PccMembershipRepository,
  PlaidService,
  PlaidSyncResult,
  SessionRepository,
  UserRepository,
} from '../../../lib/types/services.js';
import type { OrchestratorKeybridge, OrchestratorBorrowedDek } from '../../../lib/sync/orchestrator.js';

// -----------------------------------------------------------------------------
// Builders
// -----------------------------------------------------------------------------

function buildItem(args: { id: string; domain: 'personal' | 'pcc'; userId: string | null }): Item {
  return {
    id: ItemId(args.id),
    domain: args.domain,
    userId: args.userId === null ? null : UserId(args.userId),
    plaidItemId: PlaidItemId('plaid_' + args.id),
    plaidInstitutionId: null,
    institutionName: null,
    syncCursor: null,
    lastSyncAt: null,
    lastSyncOutcome: null,
    consecutiveFailures: 0,
    createdAt: new Date(),
    updatedAt: new Date(),
    removedAt: null,
    removedReason: null,
  };
}
const PlaidItemId = (s: string): import('../../../lib/types/domain.js').PlaidItemId =>
  s as unknown as import('../../../lib/types/domain.js').PlaidItemId;

function buildUser(args: { id: string }): User {
  return {
    id: UserId(args.id),
    email: `${args.id}@example.com`,
    displayName: args.id,
    role: 'member',
    userDekVersion: 1,
    createdAt: new Date(),
    updatedAt: new Date(),
  };
}

function buildSession(args: { id: string; userId: string }): Session {
  return {
    id: SessionId(args.id),
    userId: UserId(args.userId),
    status: 'active',
    createdAt: new Date(),
    lastActivityAt: new Date(),
    expiresAt: new Date(Date.now() + 3600_000),
    idleTimeoutAt: new Date(Date.now() + 1800_000),
    revokedAt: null,
    revokedReason: null,
    userAgent: null,
    remoteAddr: null,
  };
}

// -----------------------------------------------------------------------------
// Mocks
// -----------------------------------------------------------------------------

interface MockState {
  items: ReadonlyArray<Item>;
  users: ReadonlyArray<User>;
  activeSessions: Map<string, Session>;
  syncCalls: string[];
  snapshotsWritten: number;
  pccMembers: Set<string>;
}

function buildMocks(state: MockState) {
  const itemRepo: ItemRepository = {
    list: async (scope, filter) => {
      let items = state.items.filter((i) => i.removedAt === null);
      if (filter?.domain !== undefined) {
        items = items.filter((i) => i.domain === filter.domain);
      }
      if (scope.kind === 'personal') {
        items = items.filter((i) => i.domain === 'personal' && i.userId === scope.userId);
      } else if (scope.kind === 'pcc') {
        items = items.filter((i) => i.domain === 'pcc');
      }
      return Ok(items);
    },
    findById: async (scope, id) => {
      const found = state.items.find((i) => i.id === id) ?? null;
      if (found === null) {
        return Ok(null);
      }
      if (scope.kind === 'personal' && (found.domain !== 'personal' || found.userId !== scope.userId)) {
        return Ok(null);
      }
      if (scope.kind === 'pcc' && found.domain !== 'pcc') {
        return Ok(null);
      }
      return Ok(found);
    },
    create: async () => Ok({} as never),
    readEncryptedToken: async () => Ok(new Uint8Array() as never),
    rewriteEncryptedToken: async () => Ok(undefined),
    updateSyncCursor: async () => Ok(undefined),
    softRemove: async () => Ok(undefined),
  };
  const userRepo: UserRepository = {
    findByEmail: async () => Ok(null),
    findById: async () => Ok(null),
    list: async () => Ok(state.users),
    create: async () => Ok({} as never),
    setWrappedUserDek: async () => Ok(undefined),
  };
  const sessionRepo: SessionRepository = {
    create: async () => Ok({} as never),
    findActiveById: async () => Ok(null),
    findActiveByUser: async (userId) => {
      const u = userId as unknown as string;
      return Ok(state.activeSessions.get(u) ?? null);
    },
    touch: async () => Ok(undefined),
    revoke: async () => Ok(undefined),
    revokeAllActive: async () => Ok({ count: 0 }),
    expireOverdue: async () => Ok({ count: 0 }),
  };
  const pccMembershipRepo: PccMembershipRepository = {
    isActiveMember: async (userId) => Ok(state.pccMembers.has(userId as unknown as string)),
    list: async () => Ok([]),
    add: async () => Ok({} as never),
    revoke: async () => Ok(undefined),
  };
  const plaid: PlaidService = {
    mintLinkToken: async () => Err({ kind: 'plaid_api_error', httpStatus: 0, errorCode: 'noop' }),
    exchangePublicToken: async () => Err({ kind: 'plaid_api_error', httpStatus: 0, errorCode: 'noop' }),
    syncItem: async ({ itemId }) => {
      state.syncCalls.push(itemId as unknown as string);
      const r: PlaidSyncResult = { added: 1, modified: 0, removed: 0, newCursor: 'c', hasMore: false };
      return Ok(r);
    },
    refreshBalances: async () => Ok({ accountsUpdated: 0 }),
    removeItem: async () => Ok(undefined),
  };
  const snapshotWriter = {
    writeSnapshot: async () => {
      state.snapshotsWritten += 1;
      return Ok({ snapshotId: 'snap_x', netWorthCents: 0n });
    },
  } as unknown as ReturnType<typeof import('../../../lib/sync/snapshot-writer.js').createSnapshotWriter>;
  const keybridge: OrchestratorKeybridge = {
    isConnected: () => true,
    connect: async () => Ok(undefined),
    requestDek: async () => {
      const borrowed: OrchestratorBorrowedDek = {
        bytes: new Uint8Array(32),
        release: async () => undefined,
      };
      return Ok(borrowed);
    },
  };
  return { itemRepo, userRepo, sessionRepo, pccMembershipRepo, plaid, snapshotWriter, keybridge };
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

describe('SyncOrchestrator.runOnce', () => {
  it('processes PCC items even when no users have active sessions', async () => {
    const state: MockState = {
      items: [
        buildItem({ id: 'itm_pcc1', domain: 'pcc', userId: null }),
        buildItem({ id: 'itm_pcc2', domain: 'pcc', userId: null }),
      ],
      users: [buildUser({ id: 'usr_rory' })],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.runOnce({ now: new Date() });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.value.itemsAttempted).toBe(2);
      expect(r.value.itemsSucceeded).toBe(2);
    }
    expect(state.syncCalls.sort()).toEqual(['itm_pcc1', 'itm_pcc2']);
    expect(state.snapshotsWritten).toBe(1);
  });

  it('skips personal items for users with no active session', async () => {
    const state: MockState = {
      items: [
        buildItem({ id: 'itm_p1', domain: 'personal', userId: 'usr_rory' }),
        buildItem({ id: 'itm_p2', domain: 'personal', userId: 'usr_tristan' }),
      ],
      users: [buildUser({ id: 'usr_rory' }), buildUser({ id: 'usr_tristan' })],
      activeSessions: new Map(), // no one is logged in
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.runOnce({ now: new Date() });
    expect(r.ok).toBe(true);
    expect(state.syncCalls).toEqual([]);
    expect(state.snapshotsWritten).toBe(0);
  });

  it('syncs personal items only for users with an active session', async () => {
    const state: MockState = {
      items: [
        buildItem({ id: 'itm_pr1', domain: 'personal', userId: 'usr_rory' }),
        buildItem({ id: 'itm_pt1', domain: 'personal', userId: 'usr_tristan' }),
      ],
      users: [buildUser({ id: 'usr_rory' }), buildUser({ id: 'usr_tristan' })],
      activeSessions: new Map([
        ['usr_rory', buildSession({ id: 'sess_r', userId: 'usr_rory' })],
      ]),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.runOnce({ now: new Date() });
    expect(r.ok).toBe(true);
    // Only Rory's item is synced; Tristan has no session.
    expect(state.syncCalls).toEqual(['itm_pr1']);
    // One snapshot for Rory.
    expect(state.snapshotsWritten).toBe(1);
  });

  it('returns keybridge_unavailable when keybridge cannot connect', async () => {
    const state: MockState = {
      items: [],
      users: [],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    m.keybridge.isConnected = (): boolean => false;
    m.keybridge.connect = async () => Err({ kind: 'socket_unavailable' });
    const orch = createSyncOrchestrator(m);
    const r = await orch.runOnce({ now: new Date() });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('keybridge_unavailable');
    }
  });

  it('counts all PCC items as failed when DEK borrow fails for the cycle', async () => {
    const state: MockState = {
      items: [
        buildItem({ id: 'itm_pcc1', domain: 'pcc', userId: null }),
        buildItem({ id: 'itm_pcc2', domain: 'pcc', userId: null }),
      ],
      users: [],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    let pccCalls = 0;
    m.keybridge.requestDek = async (input) => {
      if ('kind' in input && input.kind === 'pcc') {
        pccCalls += 1;
        return Err({ kind: 'dek_unavailable' });
      }
      return Err({ kind: 'session_invalid' });
    };
    const orch = createSyncOrchestrator(m);
    const r = await orch.runOnce({ now: new Date() });
    expect(pccCalls).toBe(1);
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('partial_failure');
    }
    expect(state.syncCalls).toEqual([]);
  });
});

describe('SyncOrchestrator.syncItem (manual)', () => {
  it('finds a personal item under the initiator scope', async () => {
    const state: MockState = {
      items: [buildItem({ id: 'itm_p1', domain: 'personal', userId: 'usr_rory' })],
      users: [buildUser({ id: 'usr_rory' })],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.syncItem({
      itemId: ItemId('itm_p1'),
      initiatorUserId: UserId('usr_rory'),
    });
    expect(r.ok).toBe(true);
    expect(state.syncCalls).toEqual(['itm_p1']);
  });

  it('returns item_not_found for an item out of scope', async () => {
    const state: MockState = {
      items: [buildItem({ id: 'itm_p1', domain: 'personal', userId: 'usr_other' })],
      users: [],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.syncItem({
      itemId: ItemId('itm_p1'),
      initiatorUserId: UserId('usr_rory'),
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('unexpected');
    }
  });

  it('finds a PCC item when initiator is an active PCC member', async () => {
    const state: MockState = {
      items: [buildItem({ id: 'itm_pcc', domain: 'pcc', userId: null })],
      users: [],
      activeSessions: new Map(),
      syncCalls: [],
      snapshotsWritten: 0,
      pccMembers: new Set(['usr_rory']),
    };
    const m = buildMocks(state);
    const orch = createSyncOrchestrator(m);
    const r = await orch.syncItem({
      itemId: ItemId('itm_pcc'),
      initiatorUserId: UserId('usr_rory'),
    });
    expect(r.ok).toBe(true);
    expect(state.syncCalls).toEqual(['itm_pcc']);
  });
});
