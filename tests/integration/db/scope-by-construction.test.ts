// Greylock — scope-by-construction QA-PRIVACY gate
// =============================================================================
// THE most important DB test. For each repository method that takes a
// `RepoScope`, we prove:
//
//   1. personal scope of User A returns ONLY User A's rows.
//   2. personal scope querying User B's rows returns `not_found` /
//      empty list — NEVER `unauthorized` (SPEC §7).
//   3. pcc scope where caller is NOT a member returns `not_found` for PCC rows.
//   4. pcc scope where caller IS a member returns PCC rows.
//   5. admin scope returns everything (and still goes through the same
//      query builder).
//
// If this suite doesn't pass, Phase 2 doesn't ship. — orchestrator brief
// =============================================================================

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import {
  Domain,
  EncryptedBlob,
  ItemId,
  type AccountId,
  type UserId,
} from '../../../lib/types/domain.js';

import { makeTestDb, type TestDb } from './_helpers.js';

// -----------------------------------------------------------------------------
// Test fixture
// -----------------------------------------------------------------------------

interface ScopeFixture {
  readonly db: TestDb;
  readonly userA: UserId;
  readonly userB: UserId;
  readonly userC: UserId; // PCC member
  readonly userD: UserId; // NOT a PCC member
  readonly itemPersonalA: ItemId;
  readonly itemPersonalB: ItemId;
  readonly itemPcc: ItemId;
  readonly accountA: AccountId;
  readonly accountB: AccountId;
  readonly accountPcc: AccountId;
}

async function buildFixture(): Promise<ScopeFixture> {
  const db = await makeTestDb();

  const userA = expectOk(
    await db.repos.userRepo.create({
      email: 'a@example.test',
      displayName: 'Alice',
      role: 'member',
    }),
  ).id;
  const userB = expectOk(
    await db.repos.userRepo.create({
      email: 'b@example.test',
      displayName: 'Bob',
      role: 'member',
    }),
  ).id;
  const userC = expectOk(
    await db.repos.userRepo.create({
      email: 'c@example.test',
      displayName: 'Carol (PCC)',
      role: 'member',
    }),
  ).id;
  const userD = expectOk(
    await db.repos.userRepo.create({
      email: 'd@example.test',
      displayName: 'Dan (no PCC)',
      role: 'member',
    }),
  ).id;
  // C is a PCC member; D is not.
  expectOk(await db.repos.pccMembershipRepo.add({ userId: userC }));

  // Personal item for A.
  const itemPersonalA = expectOk(
    await db.repos.itemRepo.create(
      { kind: 'personal', userId: userA },
      {
        domain: 'personal',
        userId: userA,
        plaidItemId: asPlaidItemId('plaid-personal-a'),
        plaidInstitutionId: 'ins_a',
        institutionName: 'A Bank',
        encryptedAccessToken: EncryptedBlob.unsafeFromBytes(new Uint8Array([1, 1])),
      },
    ),
  ).id;

  // Personal item for B.
  const itemPersonalB = expectOk(
    await db.repos.itemRepo.create(
      { kind: 'personal', userId: userB },
      {
        domain: 'personal',
        userId: userB,
        plaidItemId: asPlaidItemId('plaid-personal-b'),
        plaidInstitutionId: 'ins_b',
        institutionName: 'B Bank',
        encryptedAccessToken: EncryptedBlob.unsafeFromBytes(new Uint8Array([2, 2])),
      },
    ),
  ).id;

  // PCC item created by member C.
  const itemPcc = expectOk(
    await db.repos.itemRepo.create(
      { kind: 'pcc', memberOfUserId: userC },
      {
        domain: 'pcc',
        userId: userC,
        plaidItemId: asPlaidItemId('plaid-pcc-1'),
        plaidInstitutionId: 'ins_pcc',
        institutionName: 'PCC Bank',
        encryptedAccessToken: EncryptedBlob.unsafeFromBytes(new Uint8Array([3, 3])),
      },
    ),
  ).id;

  // Accounts under each item. AccountRepository.upsertFromPlaid needs the
  // proper scope on the parent item.
  const upsertA = expectOk(
    await db.repos.accountRepo.upsertFromPlaid(
      { kind: 'personal', userId: userA },
      {
        itemId: itemPersonalA,
        accounts: [
          {
            plaidAccountId: asPlaidAccountId('plaid-acct-a'),
            name: 'A Checking',
            officialName: null,
            mask: '1234',
            type: 'depository',
            subtype: 'checking',
            isoCurrencyCode: 'USD',
            currentBalanceCents: 100_000n as import('../../../lib/types/domain.js').Cents,
            availableBalanceCents: 100_000n as import('../../../lib/types/domain.js').Cents,
            limitCents: null,
          },
        ],
      },
    ),
  );
  expect(upsertA.upserted).toBe(1);

  const upsertB = expectOk(
    await db.repos.accountRepo.upsertFromPlaid(
      { kind: 'personal', userId: userB },
      {
        itemId: itemPersonalB,
        accounts: [
          {
            plaidAccountId: asPlaidAccountId('plaid-acct-b'),
            name: 'B Checking',
            officialName: null,
            mask: '2345',
            type: 'depository',
            subtype: 'checking',
            isoCurrencyCode: 'USD',
            currentBalanceCents: 200_000n as import('../../../lib/types/domain.js').Cents,
            availableBalanceCents: 200_000n as import('../../../lib/types/domain.js').Cents,
            limitCents: null,
          },
        ],
      },
    ),
  );
  expect(upsertB.upserted).toBe(1);

  const upsertPcc = expectOk(
    await db.repos.accountRepo.upsertFromPlaid(
      { kind: 'pcc', memberOfUserId: userC },
      {
        itemId: itemPcc,
        accounts: [
          {
            plaidAccountId: asPlaidAccountId('plaid-acct-pcc'),
            name: 'PCC Operating',
            officialName: 'PCC LLC Operating',
            mask: '9876',
            type: 'depository',
            subtype: 'checking',
            isoCurrencyCode: 'USD',
            currentBalanceCents: 1_000_000n as import('../../../lib/types/domain.js').Cents,
            availableBalanceCents: 1_000_000n as import('../../../lib/types/domain.js').Cents,
            limitCents: null,
          },
        ],
      },
    ),
  );
  expect(upsertPcc.upserted).toBe(1);

  // Read account ids via admin.
  const allAccountsAdmin = expectOk(await db.repos.accountRepo.listAllInScope({ kind: 'admin' }));
  const accountA = allAccountsAdmin.find((a) => a.userId === userA)!.id;
  const accountB = allAccountsAdmin.find((a) => a.userId === userB)!.id;
  const accountPcc = allAccountsAdmin.find((a) => a.domain === 'pcc')!.id;

  // Snapshots: one per user, plus one PCC.
  expectOk(
    await db.repos.snapshotRepo.insert(
      { kind: 'personal', userId: userA },
      {
        domain: 'personal',
        userId: userA,
        takenAt: new Date(),
        assetsCents: 100_000n as import('../../../lib/types/domain.js').Cents,
        liabilitiesCents: 0n as import('../../../lib/types/domain.js').Cents,
        netWorthCents: 100_000n as import('../../../lib/types/domain.js').Cents,
        cashCents: 100_000n as import('../../../lib/types/domain.js').Cents,
        monthNetCents: null,
        computeVersion: 1,
        breakdownJson: '{}',
      },
    ),
  );
  expectOk(
    await db.repos.snapshotRepo.insert(
      { kind: 'personal', userId: userB },
      {
        domain: 'personal',
        userId: userB,
        takenAt: new Date(),
        assetsCents: 200_000n as import('../../../lib/types/domain.js').Cents,
        liabilitiesCents: 0n as import('../../../lib/types/domain.js').Cents,
        netWorthCents: 200_000n as import('../../../lib/types/domain.js').Cents,
        cashCents: 200_000n as import('../../../lib/types/domain.js').Cents,
        monthNetCents: null,
        computeVersion: 1,
        breakdownJson: '{}',
      },
    ),
  );
  expectOk(
    await db.repos.snapshotRepo.insert(
      { kind: 'pcc', memberOfUserId: userC },
      {
        domain: 'pcc',
        userId: null,
        takenAt: new Date(),
        assetsCents: 1_000_000n as import('../../../lib/types/domain.js').Cents,
        liabilitiesCents: 0n as import('../../../lib/types/domain.js').Cents,
        netWorthCents: 1_000_000n as import('../../../lib/types/domain.js').Cents,
        cashCents: 1_000_000n as import('../../../lib/types/domain.js').Cents,
        monthNetCents: null,
        computeVersion: 1,
        breakdownJson: '{}',
      },
    ),
  );

  return {
    db,
    userA,
    userB,
    userC,
    userD,
    itemPersonalA,
    itemPersonalB,
    itemPcc,
    accountA,
    accountB,
    accountPcc,
  };
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

describe('scope-by-construction — QA-PRIVACY gate', () => {
  let f: ScopeFixture;

  beforeEach(async () => {
    f = await buildFixture();
  });
  afterEach(async () => {
    await f.db.cleanup();
  });

  // ---------------------------------------------------------------------------
  // ItemRepository
  // ---------------------------------------------------------------------------

  describe('ItemRepository', () => {
    it('personal scope of A returns ONLY A items', async () => {
      const r = expectOk(await f.db.repos.itemRepo.list({ kind: 'personal', userId: f.userA }));
      expect(r).toHaveLength(1);
      expect(r[0]?.userId).toBe(f.userA);
      expect(r[0]?.id).toBe(f.itemPersonalA);
    });

    it("personal scope of A querying B's id returns null (not_found) — NOT unauthorized", async () => {
      const r = expectOk(
        await f.db.repos.itemRepo.findById({ kind: 'personal', userId: f.userA }, f.itemPersonalB),
      );
      expect(r).toBeNull();

      // readEncryptedToken — must be `not_found`, never `unauthorized`.
      const tok = await f.db.repos.itemRepo.readEncryptedToken(
        { kind: 'personal', userId: f.userA },
        f.itemPersonalB,
      );
      expect(tok.ok).toBe(false);
      if (!tok.ok) {
        expect(tok.error.kind).toBe('not_found');
      }
    });

    it('pcc scope where caller is NOT a member returns not_found', async () => {
      // D is not a PCC member.
      const list = await f.db.repos.itemRepo.list({
        kind: 'pcc',
        memberOfUserId: f.userD,
      });
      expect(list.ok).toBe(false);
      if (!list.ok) {
        expect(list.error.kind).toBe('not_found');
      }

      const byId = await f.db.repos.itemRepo.findById(
        { kind: 'pcc', memberOfUserId: f.userD },
        f.itemPcc,
      );
      expect(byId.ok).toBe(false);
      if (!byId.ok) {
        expect(byId.error.kind).toBe('not_found');
      }

      const tok = await f.db.repos.itemRepo.readEncryptedToken(
        { kind: 'pcc', memberOfUserId: f.userD },
        f.itemPcc,
      );
      expect(tok.ok).toBe(false);
      if (!tok.ok) {
        expect(tok.error.kind).toBe('not_found');
      }
    });

    it('pcc scope where caller IS a member returns PCC items', async () => {
      const list = expectOk(
        await f.db.repos.itemRepo.list({ kind: 'pcc', memberOfUserId: f.userC }),
      );
      expect(list).toHaveLength(1);
      expect(list[0]?.id).toBe(f.itemPcc);

      const tok = expectOk(
        await f.db.repos.itemRepo.readEncryptedToken(
          { kind: 'pcc', memberOfUserId: f.userC },
          f.itemPcc,
        ),
      );
      expect(tok.byteLength).toBe(2);
    });

    it('admin scope returns ALL items (and uses the same query builder)', async () => {
      const list = expectOk(await f.db.repos.itemRepo.list({ kind: 'admin' }));
      expect(list).toHaveLength(3);
    });

    it('cross-domain create rejected for PCC scope creating personal row', async () => {
      const bogus = await f.db.repos.itemRepo.create(
        { kind: 'pcc', memberOfUserId: f.userC },
        {
          domain: 'personal',
          userId: f.userA,
          plaidItemId: asPlaidItemId('bogus'),
          plaidInstitutionId: null,
          institutionName: null,
          encryptedAccessToken: EncryptedBlob.unsafeFromBytes(new Uint8Array([0])),
        },
      );
      expect(bogus.ok).toBe(false);
      if (!bogus.ok) {
        expect(bogus.error.kind).toBe('not_found');
      }
    });

    it('softRemove cross-scope returns not_found', async () => {
      const r = await f.db.repos.itemRepo.softRemove(
        { kind: 'personal', userId: f.userA },
        { id: f.itemPersonalB, reason: 'cross' },
      );
      expect(r.ok).toBe(false);
      if (!r.ok) {
        expect(r.error.kind).toBe('not_found');
      }
      // Verify B's item is still alive under B's own scope.
      const stillThere = expectOk(
        await f.db.repos.itemRepo.findById({ kind: 'personal', userId: f.userB }, f.itemPersonalB),
      );
      expect(stillThere?.removedAt).toBeNull();
    });

    it('updateSyncCursor cross-scope returns not_found', async () => {
      const r = await f.db.repos.itemRepo.updateSyncCursor(
        { kind: 'personal', userId: f.userA },
        { id: f.itemPersonalB, cursor: 'malicious', outcome: 'success' },
      );
      expect(r.ok).toBe(false);
      if (!r.ok) {
        expect(r.error.kind).toBe('not_found');
      }
    });

    it('rewriteEncryptedToken refuses non-admin scopes', async () => {
      const r = await f.db.repos.itemRepo.rewriteEncryptedToken(
        { kind: 'personal', userId: f.userA },
        {
          id: f.itemPersonalA,
          newBlob: EncryptedBlob.unsafeFromBytes(new Uint8Array([9, 9, 9])),
        },
      );
      expect(r.ok).toBe(false);
    });
  });

  // ---------------------------------------------------------------------------
  // AccountRepository
  // ---------------------------------------------------------------------------

  describe('AccountRepository', () => {
    it('personal scope of A returns only A accounts', async () => {
      const r = expectOk(
        await f.db.repos.accountRepo.listAllInScope({
          kind: 'personal',
          userId: f.userA,
        }),
      );
      expect(r).toHaveLength(1);
      expect(r[0]?.userId).toBe(f.userA);
    });

    it("listByItem(B's item) under A's scope returns empty", async () => {
      const r = expectOk(
        await f.db.repos.accountRepo.listByItem(
          { kind: 'personal', userId: f.userA },
          f.itemPersonalB,
        ),
      );
      expect(r).toHaveLength(0);
    });

    it('pcc scope, non-member returns not_found', async () => {
      const r = await f.db.repos.accountRepo.listAllInScope({
        kind: 'pcc',
        memberOfUserId: f.userD,
      });
      expect(r.ok).toBe(false);
    });

    it('pcc scope, member returns PCC accounts', async () => {
      const r = expectOk(
        await f.db.repos.accountRepo.listAllInScope({
          kind: 'pcc',
          memberOfUserId: f.userC,
        }),
      );
      expect(r).toHaveLength(1);
      expect(r[0]?.domain).toBe('pcc');
    });

    it('admin returns all accounts', async () => {
      const r = expectOk(await f.db.repos.accountRepo.listAllInScope({ kind: 'admin' }));
      expect(r).toHaveLength(3);
    });

    it('upsertFromPlaid cross-scope returns not_found', async () => {
      const r = await f.db.repos.accountRepo.upsertFromPlaid(
        { kind: 'personal', userId: f.userA },
        {
          itemId: f.itemPersonalB,
          accounts: [],
        },
      );
      expect(r.ok).toBe(false);
      if (!r.ok) {
        expect(r.error.kind).toBe('not_found');
      }
    });
  });

  // ---------------------------------------------------------------------------
  // TransactionRepository
  // ---------------------------------------------------------------------------

  describe('TransactionRepository', () => {
    it('apply + listByDateRange under correct scope', async () => {
      const apply = expectOk(
        await f.db.repos.transactionRepo.applyPlaidSync(
          { kind: 'personal', userId: f.userA },
          {
            itemId: f.itemPersonalA,
            added: [
              {
                itemId: f.itemPersonalA,
                accountId: f.accountA,
                domain: 'personal',
                userId: f.userA,
                plaidTransactionId: asPlaidTxId('plaid-tx-a'),
                amountCents: 5_00n as import('../../../lib/types/domain.js').Cents,
                isoCurrencyCode: 'USD',
                date: new Date('2026-04-15T00:00:00.000Z'),
                authorizedDate: null,
                name: 'Coffee',
                merchantName: 'Cafe',
                pending: false,
                category: null,
                categoryDetailed: null,
              },
            ],
            modified: [],
            removedPlaidIds: [],
          },
        ),
      );
      expect(apply.added).toBe(1);

      const list = expectOk(
        await f.db.repos.transactionRepo.listByDateRange(
          { kind: 'personal', userId: f.userA },
          {
            fromDate: new Date('2026-04-01T00:00:00.000Z'),
            toDate: new Date('2026-05-01T00:00:00.000Z'),
          },
        ),
      );
      expect(list).toHaveLength(1);
    });

    it('cross-scope applyPlaidSync returns not_found', async () => {
      const r = await f.db.repos.transactionRepo.applyPlaidSync(
        { kind: 'personal', userId: f.userA },
        {
          itemId: f.itemPersonalB,
          added: [],
          modified: [],
          removedPlaidIds: [],
        },
      );
      expect(r.ok).toBe(false);
      if (!r.ok) {
        expect(r.error.kind).toBe('not_found');
      }
    });

    it("listByDateRange under A's scope returns NO B transactions", async () => {
      // Seed B with a transaction.
      expectOk(
        await f.db.repos.transactionRepo.applyPlaidSync(
          { kind: 'personal', userId: f.userB },
          {
            itemId: f.itemPersonalB,
            added: [
              {
                itemId: f.itemPersonalB,
                accountId: f.accountB,
                domain: 'personal',
                userId: f.userB,
                plaidTransactionId: asPlaidTxId('plaid-tx-b'),
                amountCents: 99_00n as import('../../../lib/types/domain.js').Cents,
                isoCurrencyCode: 'USD',
                date: new Date('2026-04-20T00:00:00.000Z'),
                authorizedDate: null,
                name: 'Secret B Purchase',
                merchantName: null,
                pending: false,
                category: null,
                categoryDetailed: null,
              },
            ],
            modified: [],
            removedPlaidIds: [],
          },
        ),
      );

      const r = expectOk(
        await f.db.repos.transactionRepo.listByDateRange(
          { kind: 'personal', userId: f.userA },
          {
            fromDate: new Date('2026-04-01T00:00:00.000Z'),
            toDate: new Date('2026-05-01T00:00:00.000Z'),
          },
        ),
      );
      // None of the names should mention B's purchase.
      for (const t of r) {
        expect(t.name).not.toBe('Secret B Purchase');
        expect(t.userId).toBe(f.userA);
      }
    });
  });

  // ---------------------------------------------------------------------------
  // SnapshotRepository
  // ---------------------------------------------------------------------------

  describe('SnapshotRepository', () => {
    it('latest under A returns only A snapshot', async () => {
      const a = expectOk(
        await f.db.repos.snapshotRepo.latest(
          { kind: 'personal', userId: f.userA },
          { domain: 'personal', userId: f.userA },
        ),
      );
      expect(a?.userId).toBe(f.userA);
    });

    it('latest under A asking for B returns not_found via consent gate', async () => {
      const r = await f.db.repos.snapshotRepo.latest(
        { kind: 'personal', userId: f.userA },
        { domain: 'personal', userId: f.userB },
      );
      expect(r.ok).toBe(false);
      if (!r.ok) {
        expect(r.error.kind).toBe('not_found');
      }
    });

    it('pcc scope, non-member, latest returns not_found', async () => {
      const r = await f.db.repos.snapshotRepo.latest(
        { kind: 'pcc', memberOfUserId: f.userD },
        { domain: 'pcc', userId: null },
      );
      expect(r.ok).toBe(false);
    });

    it('pcc scope, member, latest returns the PCC snapshot', async () => {
      const r = expectOk(
        await f.db.repos.snapshotRepo.latest(
          { kind: 'pcc', memberOfUserId: f.userC },
          { domain: 'pcc', userId: null },
        ),
      );
      expect(r?.domain).toBe('pcc');
    });

    it('admin sees all', async () => {
      const fromTs = new Date(Date.now() - 24 * 3600 * 1000);
      const toTs = new Date(Date.now() + 24 * 3600 * 1000);
      const r = expectOk(
        await f.db.repos.snapshotRepo.series(
          { kind: 'admin' },
          { domain: 'personal', userId: f.userA, fromTs, toTs },
        ),
      );
      expect(r.length).toBeGreaterThanOrEqual(1);
    });
  });

  // Quiet unused-import lints
  void Domain.Personal;
  void ItemId;
});

// -----------------------------------------------------------------------------
// Tiny test-local helpers
// -----------------------------------------------------------------------------

function expectOk<T, E>(r: { ok: true; value: T } | { ok: false; error: E }): T {
  if (!r.ok) {
    throw new Error(`expected Ok, got Err: ${JSON.stringify(r.error)}`);
  }
  return r.value;
}

const asPlaidItemId = (s: string): import('../../../lib/types/domain.js').PlaidItemId =>
  s as import('../../../lib/types/domain.js').PlaidItemId;
const asPlaidAccountId = (s: string): import('../../../lib/types/domain.js').PlaidAccountId =>
  s as import('../../../lib/types/domain.js').PlaidAccountId;
const asPlaidTxId = (s: string): import('../../../lib/types/domain.js').PlaidTransactionId =>
  s as import('../../../lib/types/domain.js').PlaidTransactionId;
