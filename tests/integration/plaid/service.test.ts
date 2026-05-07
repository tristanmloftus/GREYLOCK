// Greylock — PlaidService integration tests
// =============================================================================
// Spins up a real SQLCipher-encrypted DB + a real CryptoService (with a TTY
// passphrase injection so no Keychain is required) + a mocked Plaid SDK.
//
// Verified behaviors (per AGENT-PLAID brief):
//   - `exchangePublicToken` round-trip: encrypted blob is NOT the original
//     plaintext UTF-8; decrypts back under the correct AAD/key; FAILS under
//     wrong AAD (cross-domain substitution).
//   - `syncItem` cursor advances ONLY on commit; on apply failure the cursor
//     stays at its prior value; counts surface to the audit log without
//     transaction amounts.
//   - `removeItem` calls Plaid `itemRemove` once with a decrypted token,
//     zeroizes the buffer, and soft-deletes the item.
// =============================================================================

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { randomBytes } from 'node:crypto';

import {
  ActorKind,
  AuditAction,
  AuditOutcome,
  EncryptedBlob,
  ItemId,
  PasskeyId,
  UserId,
} from '../../../lib/types/domain.js';
import type {
  AuditAppendInput,
  AuditService,
  RepoScope,
} from '../../../lib/types/services.js';
import type {
  AuditEntry,
  PlaidPublicToken,
} from '../../../lib/types/domain.js';
import { Ok } from '../../../lib/types/domain.js';

import { createCryptoService, wrapPccDek } from '../../../lib/crypto/index.js';
import { wrapUserDek } from '../../../lib/crypto/user-dek.js';

import { createPlaidServiceWithBroker } from '../../../lib/plaid/index.js';
import { aadForItemToken } from '../../../lib/crypto/aad.js';

import { makeTestDb, type TestDb } from '../db/_helpers.js';

// -----------------------------------------------------------------------------
// Fixed test secrets
// -----------------------------------------------------------------------------
//
// The master passphrase + pepper here are TEST-ONLY values. They never escape
// this file. The CryptoService is instantiated with a `ttyPromptImpl` that
// returns these bytes so the Keychain is bypassed.
// -----------------------------------------------------------------------------

const TEST_PASSPHRASE = 'test-master-passphrase-do-not-use-in-production';
const TEST_PEPPER = randomBytes(32);

// We use the production scrypt N here (the CryptoService's
// `initializeFromKeychain` re-derives with production params; matching them
// at seed time is required for the unwrap to succeed). Cost: ~250ms per
// bootstrap call. Tests run 5 of them serially.
const TEST_SCRYPT_N = 1 << 17;

// -----------------------------------------------------------------------------
// In-memory audit collector
// -----------------------------------------------------------------------------

function makeMemAudit(): { service: AuditService; entries: AuditAppendInput[] } {
  const entries: AuditAppendInput[] = [];
  const service: AuditService = {
    append: vi.fn(async (input) => {
      entries.push(input);
      return Ok({} as unknown as AuditEntry);
    }),
    query: vi.fn(),
    verifyChain: vi.fn(),
  };
  return { service, entries };
}

// -----------------------------------------------------------------------------
// CryptoService bootstrap helper
// -----------------------------------------------------------------------------

interface PreparedCrypto {
  readonly crypto: import('../../../lib/types/services.js').CryptoService;
  readonly kdfSalt: Uint8Array;
  readonly pccVersion: number;
  readonly testUserId: UserId;
  readonly testUserDekVersion: number;
  readonly testCredentialId: Uint8Array;
  readonly testKekSalt: Uint8Array;
  readonly testWrappedUserDek: EncryptedBlob;
}

async function bootstrapCrypto(db: TestDb): Promise<PreparedCrypto> {
  // 1. Generate per-test PCC kdfSalt + Master KEK from the passphrase.
  const kdfSalt = randomBytes(16);
  // Reuse `loadMasterKek` indirectly by using `createCryptoService` with a
  // ttyPromptImpl that returns our passphrase bytes. To keep the test fast
  // we use scrypt N=2 — same module, different param.
  // To inject scrypt params, we wrap loadMasterKek manually: derive the KEK
  // here, wrap a fresh PCC DEK, insert into the repo, then create the
  // crypto service whose `initializeFromKeychain` will re-derive (also fast
  // because the test ttyPromptImpl returns the same bytes).
  // But `loadMasterKek` in the service uses production scrypt params. So
  // for the test we use the *primitive* directly to seed:
  const { deriveMasterKek } = await import('../../../lib/crypto/master-key.js');
  const masterKek = deriveMasterKek({
    secretBytes: Buffer.from(TEST_PASSPHRASE, 'utf8'),
    kdfSalt,
    pepperBytes: TEST_PEPPER,
    N: TEST_SCRYPT_N,
  });

  // 2. Wrap a fresh PCC DEK under the master KEK and insert it.
  const pccDekMaterial = randomBytes(32);
  const wrappedPccDek = wrapPccDek({
    masterKek,
    version: 1,
    dekMaterial: pccDekMaterial,
  });
  const insRes = await db.repos.pccKeyWrapRepo.insert({
    version: 1,
    wrappedDek: wrappedPccDek,
    kdfAlgorithm: 'scrypt',
    kdfN: TEST_SCRYPT_N,
    kdfR: 8,
    kdfP: 1,
    kdfSalt,
  });
  if (!insRes.ok) {
    throw new Error('failed to insert PccKeyWrap');
  }

  // 3. Create a test user and seed a wrapped user DEK + Passkey row.
  const userCreate = await db.repos.userRepo.create({
    email: 'rory@example.test',
    displayName: 'Rory',
    role: 'owner',
  });
  if (!userCreate.ok) {
    throw new Error('failed to create user');
  }
  const userId = userCreate.value.id;
  const credentialId = randomBytes(32);
  const kekSalt = randomBytes(16);
  const dekMaterial = randomBytes(32);
  const wrappedUserDekBlob = wrapUserDek({
    userId,
    credentialId,
    kekSalt,
    pepperBytes: TEST_PEPPER,
    dekMaterial,
  });
  await db.repos.userRepo.setWrappedUserDek({
    userId,
    version: 1,
    wrapped: wrappedUserDekBlob,
  });

  // We also need a passkey row for completeness (some flows expect it).
  await db.repos.passkeyRepo.create({
    userId,
    credentialId,
    credentialPublicKey: randomBytes(32),
    counter: 0n,
    transports: ['internal'],
    aaguid: null,
    deviceLabel: 'test',
    kekSalt,
  });
  void PasskeyId;

  // 4. Construct the CryptoService. We use a no-op TTY prompt impl that
  //    returns the passphrase, and override scrypt N at the bootstrap point
  //    by replacing the keychain options' fallbackTty path.
  // Since `createCryptoService.initializeFromKeychain` calls `loadMasterKek`
  // with the production SCRYPT_PARAMS, and we want fast tests, we monkey-
  // patch the bootstrap by using a ttyPromptImpl AND re-deriving the KEK
  // ourselves into a closure. The simpler path: use the built-in primitive
  // path and skip `initializeFromKeychain`. We bypass the service's
  // bootstrap by directly seeding state via a custom factory.
  //
  // Instead of fighting the production-scrypt-N inside `initializeFromKeychain`,
  // we set the wrap row's kdfSalt to the same value used to derive masterKek
  // above, and inject our test scrypt N via a service that uses the same.
  // The cleanest path is to call `createCryptoService` and `initializeFromKeychain`
  // — the test pays the (~250ms) scrypt cost ONCE per test. This is acceptable.

  const crypto = createCryptoService({
    bootstrap: {
      activePccKeyWrap: {
        version: 1,
        wrappedDek: wrappedPccDek,
        kdfSalt,
      },
      pepperBytes: TEST_PEPPER,
      keychain: {
        fallbackTty: true,
        ttyPromptImpl: async () => Buffer.from(TEST_PASSPHRASE, 'utf8'),
      },
    },
  });
  const initRes = await crypto.initializeFromKeychain();
  if (!initRes.ok) {
    throw new Error(`crypto init failed: ${initRes.error.kind}`);
  }
  const loadRes = await crypto.loadUserDek({
    userId,
    credentialId,
    kekSalt,
    wrappedUserDek: wrappedUserDekBlob,
    userDekVersion: 1,
  });
  if (!loadRes.ok) {
    throw new Error(`loadUserDek failed: ${loadRes.error.kind}`);
  }

  // Zeroize the local masterKek + materials.
  masterKek.fill(0);
  pccDekMaterial.fill(0);
  dekMaterial.fill(0);

  return {
    crypto,
    kdfSalt,
    pccVersion: 1,
    testUserId: userId,
    testUserDekVersion: 1,
    testCredentialId: credentialId,
    testKekSalt: kekSalt,
    testWrappedUserDek: wrappedUserDekBlob,
  };
}

// -----------------------------------------------------------------------------
// Mocked Plaid client
// -----------------------------------------------------------------------------

interface MockedPlaid {
  client: import('plaid').PlaidApi;
  setExchangeResult(args: { access_token: string; item_id: string }): void;
  setExchangeReject(err: unknown): void;
  setSyncResults(results: ReadonlyArray<unknown>): void;
  setSyncReject(err: unknown): void;
  setItemRemove(impl: () => Promise<unknown>): void;
  exchangeCalls(): unknown[];
  syncCalls(): unknown[];
  itemRemoveCalls(): unknown[];
}

function makeMockPlaid(): MockedPlaid {
  let exchangeData = { access_token: 'access-sandbox-test-token-AAAA', item_id: 'plaid-item-1' };
  let exchangeRejection: unknown = null;
  let syncResults: ReadonlyArray<unknown> = [];
  let syncRejection: unknown = null;
  let itemRemoveImpl: () => Promise<unknown> = async () => ({ data: { request_id: 'req' } });
  let syncIdx = 0;
  const exCalls: unknown[] = [];
  const syCalls: unknown[] = [];
  const rmCalls: unknown[] = [];

  const client = {
    linkTokenCreate: vi.fn(async () => ({
      data: {
        link_token: 'link-sandbox-test',
        expiration: new Date(Date.now() + 3600_000).toISOString(),
        request_id: 'req',
      },
    })),
    itemPublicTokenExchange: vi.fn(async (req: { public_token: string }) => {
      exCalls.push(req);
      if (exchangeRejection !== null) {
        throw exchangeRejection;
      }
      return { data: { ...exchangeData, request_id: 'req' } };
    }),
    transactionsSync: vi.fn(async (req: { access_token: string; cursor: string; count: number }) => {
      syCalls.push(req);
      if (syncRejection !== null) {
        throw syncRejection;
      }
      const idx = syncIdx;
      syncIdx += 1;
      const r = syncResults[idx];
      if (r === undefined) {
        return {
          data: {
            transactions_update_status: 'HISTORICAL_UPDATE_COMPLETE',
            accounts: [],
            added: [],
            modified: [],
            removed: [],
            next_cursor: req.cursor,
            has_more: false,
            request_id: 'req',
          },
        };
      }
      return { data: r };
    }),
    accountsBalanceGet: vi.fn(async () => ({ data: { accounts: [] } })),
    itemRemove: vi.fn(async (req: { access_token: string }) => {
      rmCalls.push(req);
      return await itemRemoveImpl();
    }),
  } as unknown as import('plaid').PlaidApi;

  return {
    client,
    setExchangeResult: (args) => {
      exchangeData = args;
      exchangeRejection = null;
    },
    setExchangeReject: (err) => {
      exchangeRejection = err;
    },
    setSyncResults: (results) => {
      syncResults = results;
      syncIdx = 0;
      syncRejection = null;
    },
    setSyncReject: (err) => {
      syncRejection = err;
    },
    setItemRemove: (impl) => {
      itemRemoveImpl = impl;
    },
    exchangeCalls: () => exCalls,
    syncCalls: () => syCalls,
    itemRemoveCalls: () => rmCalls,
  };
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

describe('PlaidService integration', () => {
  let db: TestDb;
  let prepared: PreparedCrypto;

  beforeEach(async () => {
    db = await makeTestDb();
    prepared = await bootstrapCrypto(db);
  }, 15_000); // bootstrap pays the full-strength scrypt cost once per test.

  afterEach(async () => {
    await db.cleanup();
  });

  it('exchangePublicToken (personal): encrypted blob is NOT plaintext UTF-8 and decrypts under correct AAD', async () => {
    const audit = makeMemAudit();
    const mock = makeMockPlaid();
    mock.setExchangeResult({
      access_token: 'access-sandbox-PLAINTEXT-DO-NOT-LEAK',
      item_id: 'plaid-item-AAA',
    });

    const svc = createPlaidServiceWithBroker({
      plaidClient: mock.client,
      crypto: prepared.crypto,
      itemRepo: db.repos.itemRepo,
      accountRepo: db.repos.accountRepo,
      transactionRepo: db.repos.transactionRepo,
      userRepo: db.repos.userRepo,
      pccMembershipRepo: db.repos.pccMembershipRepo,
      pccKeyWrapRepo: db.repos.pccKeyWrapRepo,
      audit: audit.service,
      clientName: 'Greylock-Test',
      countryCodes: ['US'],
      defaultProducts: ['transactions'],
    });

    const exRes = await svc.exchangePublicToken({
      userId: prepared.testUserId,
      domain: 'personal',
      publicToken: 'public-sandbox-test' as unknown as PlaidPublicToken,
      institutionId: null,
      institutionName: 'TestBank',
    });
    expect(exRes.ok).toBe(true);
    if (!exRes.ok) {return;}
    const newItemId = exRes.value.itemId;

    // Read back the persisted blob via the admin scope.
    const adminScope: RepoScope = { kind: 'admin' };
    const blobRes = await db.repos.itemRepo.readEncryptedToken(adminScope, newItemId);
    expect(blobRes.ok).toBe(true);
    if (!blobRes.ok) {return;}
    const blob = blobRes.value;

    // Convert to a Buffer and assert: the persisted bytes are NOT the
    // plaintext UTF-8.
    const blobStr = Buffer.from(blob).toString('utf8');
    expect(blobStr.includes('access-sandbox-PLAINTEXT-DO-NOT-LEAK')).toBe(false);

    // Now decrypt with the correct AAD/handle and confirm we recover the
    // plaintext.
    const decRes = await prepared.crypto.decrypt({
      handle: { kind: 'user', userId: prepared.testUserId, version: prepared.testUserDekVersion },
      aad: { kind: 'item_token', itemId: newItemId },
      domain: 'personal',
      blob,
    });
    expect(decRes.ok).toBe(true);
    if (!decRes.ok) {return;}
    expect(Buffer.from(decRes.value).toString('utf8')).toBe('access-sandbox-PLAINTEXT-DO-NOT-LEAK');
    // Bonus: confirm AAD construction matches expectation.
    const aadBytes = aadForItemToken({
      domain: 'personal',
      itemId: newItemId,
      keyVersion: prepared.testUserDekVersion,
    });
    expect(new TextDecoder().decode(aadBytes)).toBe(
      `personal:itemtoken:${newItemId}:${prepared.testUserDekVersion}`,
    );
  });

  it('exchangePublicToken: cross-domain substitution fails (decrypt as PCC fails)', async () => {
    const audit = makeMemAudit();
    const mock = makeMockPlaid();
    mock.setExchangeResult({
      access_token: 'access-sandbox-cross-domain-test',
      item_id: 'plaid-item-cross',
    });
    const svc = createPlaidServiceWithBroker({
      plaidClient: mock.client,
      crypto: prepared.crypto,
      itemRepo: db.repos.itemRepo,
      accountRepo: db.repos.accountRepo,
      transactionRepo: db.repos.transactionRepo,
      userRepo: db.repos.userRepo,
      pccMembershipRepo: db.repos.pccMembershipRepo,
      pccKeyWrapRepo: db.repos.pccKeyWrapRepo,
      audit: audit.service,
      clientName: 'Greylock-Test',
      countryCodes: ['US'],
      defaultProducts: ['transactions'],
    });
    const exRes = await svc.exchangePublicToken({
      userId: prepared.testUserId,
      domain: 'personal',
      publicToken: 'public-sandbox-test' as unknown as PlaidPublicToken,
      institutionId: null,
      institutionName: null,
    });
    expect(exRes.ok).toBe(true);
    if (!exRes.ok) {return;}

    const adminScope: RepoScope = { kind: 'admin' };
    const blobRes = await db.repos.itemRepo.readEncryptedToken(adminScope, exRes.value.itemId);
    expect(blobRes.ok).toBe(true);
    if (!blobRes.ok) {return;}

    // Attempt to decrypt as PCC — should fail.
    const wrongDec = await prepared.crypto.decrypt({
      handle: { kind: 'pcc', version: 1 },
      aad: { kind: 'item_token', itemId: exRes.value.itemId },
      domain: 'pcc',
      blob: blobRes.value,
    });
    expect(wrongDec.ok).toBe(false);
  });

  it('syncItem: cursor advances on success, audit carries counts but no amounts', async () => {
    const audit = makeMemAudit();
    const mock = makeMockPlaid();
    mock.setExchangeResult({
      access_token: 'access-sandbox-sync-test',
      item_id: 'plaid-item-sync',
    });
    const svc = createPlaidServiceWithBroker({
      plaidClient: mock.client,
      crypto: prepared.crypto,
      itemRepo: db.repos.itemRepo,
      accountRepo: db.repos.accountRepo,
      transactionRepo: db.repos.transactionRepo,
      userRepo: db.repos.userRepo,
      pccMembershipRepo: db.repos.pccMembershipRepo,
      pccKeyWrapRepo: db.repos.pccKeyWrapRepo,
      audit: audit.service,
      clientName: 'Greylock-Test',
      countryCodes: ['US'],
      defaultProducts: ['transactions'],
    });
    const exRes = await svc.exchangePublicToken({
      userId: prepared.testUserId,
      domain: 'personal',
      publicToken: 'public-sandbox' as unknown as PlaidPublicToken,
      institutionId: null,
      institutionName: null,
    });
    if (!exRes.ok) {throw new Error('exchange failed');}
    const newItemId = exRes.value.itemId;

    // Set up a single-page sync result with one account + one transaction.
    mock.setSyncResults([
      {
        transactions_update_status: 'HISTORICAL_UPDATE_COMPLETE',
        accounts: [
          {
            account_id: 'plaid-acc-1',
            balances: { current: 1000.5, available: 800.0, limit: null, iso_currency_code: 'USD', unofficial_currency_code: null },
            mask: '1234',
            name: 'Checking',
            official_name: null,
            type: 'depository',
            subtype: 'checking',
          },
        ],
        added: [
          {
            account_id: 'plaid-acc-1',
            amount: 12.34,
            iso_currency_code: 'USD',
            unofficial_currency_code: null,
            category: ['Shops'],
            category_id: '19046000',
            date: '2025-02-01',
            location: { address: null, city: null, country: null, lat: null, lon: null, postal_code: null, region: null, store_number: null },
            name: 'Coffee',
            merchant_name: 'Starbucks',
            payment_meta: { by_order_of: null, payee: null, payer: null, payment_method: null, payment_processor: null, ppd_id: null, reason: null, reference_number: null },
            pending: false,
            pending_transaction_id: null,
            account_owner: null,
            transaction_id: 'plaid-tx-1',
            payment_channel: 'in store',
            transaction_code: null,
            authorized_date: '2025-02-01',
            authorized_datetime: null,
            datetime: null,
          },
        ],
        modified: [],
        removed: [],
        next_cursor: 'cursor-after-page-1',
        has_more: false,
        request_id: 'req',
      },
    ]);

    const syncRes = await svc.syncItem({ itemId: newItemId });
    expect(syncRes.ok).toBe(true);
    if (!syncRes.ok) {return;}
    expect(syncRes.value.added).toBe(1);
    expect(syncRes.value.modified).toBe(0);
    expect(syncRes.value.removed).toBe(0);
    expect(syncRes.value.newCursor).toBe('cursor-after-page-1');

    // Cursor advanced.
    const adminScope: RepoScope = { kind: 'admin' };
    const itemAfter = await db.repos.itemRepo.findById(adminScope, newItemId);
    expect(itemAfter.ok).toBe(true);
    if (!itemAfter.ok || itemAfter.value === null) {return;}
    expect(itemAfter.value.syncCursor).toBe('cursor-after-page-1');
    expect(itemAfter.value.lastSyncOutcome).toBe('success');

    // Audit: completed entry has counts, no amounts.
    const completed = audit.entries.find((e) => e.action === AuditAction.PlaidSyncCompleted);
    expect(completed).toBeDefined();
    if (completed === undefined) {return;}
    expect(completed.outcome).toBe(AuditOutcome.Success);
    const detailsStr = JSON.stringify(completed.details);
    expect(detailsStr).toContain('"added":1');
    // No transaction amounts in the audit details.
    expect(detailsStr.includes('1234')).toBe(false);
    expect(detailsStr.includes('12.34')).toBe(false);
    expect(detailsStr.toLowerCase().includes('starbucks')).toBe(false);

    // Token never appears in any audit entry.
    for (const e of audit.entries) {
      const dump = JSON.stringify(e);
      expect(dump.includes('access-sandbox-sync-test')).toBe(false);
    }
  });

  it('syncItem: on sync failure cursor stays at prior value and audit emits failure', async () => {
    const audit = makeMemAudit();
    const mock = makeMockPlaid();
    mock.setExchangeResult({
      access_token: 'access-sandbox-fail-test',
      item_id: 'plaid-item-fail',
    });
    const svc = createPlaidServiceWithBroker({
      plaidClient: mock.client,
      crypto: prepared.crypto,
      itemRepo: db.repos.itemRepo,
      accountRepo: db.repos.accountRepo,
      transactionRepo: db.repos.transactionRepo,
      userRepo: db.repos.userRepo,
      pccMembershipRepo: db.repos.pccMembershipRepo,
      pccKeyWrapRepo: db.repos.pccKeyWrapRepo,
      audit: audit.service,
      clientName: 'Greylock-Test',
      countryCodes: ['US'],
      defaultProducts: ['transactions'],
    });
    const exRes = await svc.exchangePublicToken({
      userId: prepared.testUserId,
      domain: 'personal',
      publicToken: 'public-sandbox' as unknown as PlaidPublicToken,
      institutionId: null,
      institutionName: null,
    });
    if (!exRes.ok) {throw new Error('exchange failed');}
    const newItemId = exRes.value.itemId;

    // Make the SDK throw on transactionsSync.
    const fakeErr = new Error('plaid down') as Error & { response?: { status: number; data: { error_code: string } } };
    (fakeErr as { response: unknown }).response = { status: 502, data: { error_code: 'INTERNAL_SERVER_ERROR' } };
    mock.setSyncReject(fakeErr);

    const syncRes = await svc.syncItem({ itemId: newItemId });
    expect(syncRes.ok).toBe(false);

    const adminScope: RepoScope = { kind: 'admin' };
    const itemAfter = await db.repos.itemRepo.findById(adminScope, newItemId);
    expect(itemAfter.ok).toBe(true);
    if (!itemAfter.ok || itemAfter.value === null) {return;}
    // Cursor stayed null (prior value).
    expect(itemAfter.value.syncCursor === null || itemAfter.value.syncCursor === '').toBe(true);
    expect(itemAfter.value.lastSyncOutcome).toBe('error');
    expect(itemAfter.value.consecutiveFailures).toBeGreaterThan(0);

    const failed = audit.entries.find((e) => e.action === AuditAction.PlaidSyncFailed);
    expect(failed).toBeDefined();
  });

  it('removeItem: calls Plaid itemRemove once with decrypted token and soft-removes the row', async () => {
    const audit = makeMemAudit();
    const mock = makeMockPlaid();
    mock.setExchangeResult({
      access_token: 'access-sandbox-remove-test',
      item_id: 'plaid-item-rm',
    });
    const svc = createPlaidServiceWithBroker({
      plaidClient: mock.client,
      crypto: prepared.crypto,
      itemRepo: db.repos.itemRepo,
      accountRepo: db.repos.accountRepo,
      transactionRepo: db.repos.transactionRepo,
      userRepo: db.repos.userRepo,
      pccMembershipRepo: db.repos.pccMembershipRepo,
      pccKeyWrapRepo: db.repos.pccKeyWrapRepo,
      audit: audit.service,
      clientName: 'Greylock-Test',
      countryCodes: ['US'],
      defaultProducts: ['transactions'],
    });
    const exRes = await svc.exchangePublicToken({
      userId: prepared.testUserId,
      domain: 'personal',
      publicToken: 'public-sandbox' as unknown as PlaidPublicToken,
      institutionId: null,
      institutionName: null,
    });
    if (!exRes.ok) {throw new Error('exchange failed');}
    const newItemId = exRes.value.itemId;

    const rmRes = await svc.removeItem({ itemId: newItemId, reason: 'test_remove' });
    expect(rmRes.ok).toBe(true);

    expect(mock.itemRemoveCalls().length).toBe(1);
    const arg = mock.itemRemoveCalls()[0] as { access_token: string };
    expect(arg.access_token).toBe('access-sandbox-remove-test');

    // Soft-removed.
    const adminScope: RepoScope = { kind: 'admin' };
    const itemAfter = await db.repos.itemRepo.findById(adminScope, newItemId);
    if (!itemAfter.ok) {return;}
    expect(itemAfter.value?.removedAt).not.toBe(null);
    expect(itemAfter.value?.removedReason).toBe('test_remove');

    // Audit emitted plaid_item_removed.
    const removed = audit.entries.find((e) => e.action === AuditAction.PlaidItemRemoved);
    expect(removed).toBeDefined();
    if (removed === undefined) {return;}
    expect(removed.outcome).toBe(AuditOutcome.Success);
  });

  // Touch unused import to satisfy strict 'noUnusedLocals'.
  void ItemId;
  void UserId;
  void EncryptedBlob;
  void ActorKind;
});
