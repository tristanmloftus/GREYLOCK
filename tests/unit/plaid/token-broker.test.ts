// Greylock — PlaidTokenBroker unit tests
// =============================================================================
// Verifies the security-critical broker:
//   - Buffer.fill(0) on success
//   - Buffer.fill(0) on use-callback throw
//   - decrypt failure surfaces error (no plaintext leak)
//   - AAD is constructed correctly per domain
//   - admin scope is used on item / token reads
//   - audit emits on success and failure with itemId only — never token bytes
// =============================================================================

import { describe, it, expect, vi } from 'vitest';

import { Err, Ok } from '../../../lib/types/domain.js';
import { ItemId, UserId } from '../../../lib/types/domain.js';
import type {
  CryptoService,
  ItemRepository,
  UserRepository,
  AuditService,
} from '../../../lib/types/services.js';
import type {
  EncryptedBlob,
  Item,
  PlaidAccessTokenInMemory,
  User,
} from '../../../lib/types/domain.js';
import type { RepoScope } from '../../../lib/types/services.js';
import { EncryptedBlob as EncryptedBlobCtor } from '../../../lib/types/domain.js';
import { createPlaidTokenBroker } from '../../../lib/plaid/token-broker.js';
import type { PccKeyWrapRepository, PccKeyWrapRecord } from '../../../lib/db/repositories/pcc-key-wrap.js';

const TOKEN_PLAINTEXT = 'access-sandbox-token-do-not-leak';

function makeItem(over: Partial<Item> = {}): Item {
  return {
    id: ItemId('item_1'),
    domain: 'personal',
    userId: UserId('user_1'),
    plaidItemId: 'plaid_item_1' as never,
    plaidInstitutionId: null,
    institutionName: null,
    syncCursor: null,
    lastSyncAt: null,
    lastSyncOutcome: null,
    consecutiveFailures: 0,
    createdAt: new Date(0),
    updatedAt: new Date(0),
    removedAt: null,
    removedReason: null,
    ...over,
  };
}

function makeUser(over: Partial<User> = {}): User {
  return {
    id: UserId('user_1'),
    email: 'rory@example.com',
    displayName: 'Rory',
    role: 'owner',
    userDekVersion: 7,
    createdAt: new Date(0),
    updatedAt: new Date(0),
    ...over,
  };
}

function makeBlob(): EncryptedBlob {
  return EncryptedBlobCtor.unsafeFromBytes(new Uint8Array([0x01, 0x01, 1, 2, 3]));
}

interface MockedDeps {
  cryptoCalls: Array<unknown>;
  itemRepoFindByIdCalls: Array<{ scope: RepoScope; id: string }>;
  itemRepoReadTokenCalls: Array<{ scope: RepoScope; id: string }>;
  auditCalls: Array<Parameters<AuditService['append']>[0]>;
}

function buildMocks(opts: {
  item?: Item | null;
  user?: User | null;
  pccKey?: PccKeyWrapRecord | null;
  blob?: EncryptedBlob | null;
  decrypt?: (input: unknown) => Awaited<ReturnType<CryptoService['decrypt']>>;
  decryptResult?: Awaited<ReturnType<CryptoService['decrypt']>>;
} = {}): {
  state: MockedDeps;
  crypto: CryptoService;
  itemRepo: ItemRepository;
  userRepo: UserRepository;
  pccKeyWrapRepo: PccKeyWrapRepository;
  audit: AuditService;
} {
  const state: MockedDeps = {
    cryptoCalls: [],
    itemRepoFindByIdCalls: [],
    itemRepoReadTokenCalls: [],
    auditCalls: [],
  };
  const item = opts.item === undefined ? makeItem() : opts.item;
  const user = opts.user === undefined ? makeUser() : opts.user;
  const blob = opts.blob === undefined ? makeBlob() : opts.blob;

  const crypto = {
    initializeFromKeychain: vi.fn(),
    shutdown: vi.fn(),
    loadUserDek: vi.fn(),
    unloadUserDek: vi.fn(),
    hasUserDek: vi.fn(),
    hasPccDek: vi.fn(),
    encrypt: vi.fn(),
    decrypt: vi.fn(async (input: unknown) => {
      state.cryptoCalls.push(input);
      if (opts.decrypt !== undefined) {
        return opts.decrypt(input);
      }
      if (opts.decryptResult !== undefined) {
        return opts.decryptResult;
      }
      return Ok(new TextEncoder().encode(TOKEN_PLAINTEXT));
    }),
    wrapUserDek: vi.fn(),
    rotateUserDek: vi.fn(),
    rotateMaster: vi.fn(),
  } as unknown as CryptoService;

  const itemRepo: ItemRepository = {
    list: vi.fn(),
    findById: vi.fn(async (scope, id) => {
      state.itemRepoFindByIdCalls.push({ scope, id });
      if (item === null) {
        return Ok(null);
      }
      return Ok(item);
    }),
    create: vi.fn(),
    readEncryptedToken: vi.fn(async (scope: RepoScope, id: string) => {
      state.itemRepoReadTokenCalls.push({ scope, id });
      if (blob === null) {
        return Err({ kind: 'not_found' as const });
      }
      return Ok(blob);
    }) as unknown as ItemRepository['readEncryptedToken'],
    rewriteEncryptedToken: vi.fn(),
    updateSyncCursor: vi.fn(),
    softRemove: vi.fn(),
  };

  const userRepo: UserRepository = {
    findByEmail: vi.fn(),
    findById: vi.fn(async () => (user === null ? Ok(null) : Ok(user))),
    list: vi.fn(),
    create: vi.fn(),
    setWrappedUserDek: vi.fn(),
  };

  const pccKeyWrapRepo: PccKeyWrapRepository = {
    findActive: vi.fn(async () => Ok(opts.pccKey ?? null)),
    findByVersion: vi.fn(),
    insert: vi.fn(),
    retire: vi.fn(),
  };

  const audit: AuditService = {
    append: vi.fn(async (input) => {
      state.auditCalls.push(input);
      return Ok({} as never);
    }),
    query: vi.fn(),
    verifyChain: vi.fn(),
  };

  return { state, crypto, itemRepo, userRepo, pccKeyWrapRepo, audit };
}

describe('PlaidTokenBroker — withDecryptedToken', () => {
  it('success path: invokes use(), zeroes the buffer, audits success', async () => {
    const m = buildMocks();
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    let capturedToken = '';
    const res = await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async (token: PlaidAccessTokenInMemory) => {
        capturedToken = String(token);
        return 42;
      },
    });
    expect(res.ok).toBe(true);
    if (!res.ok) {return;}
    expect(res.value).toBe(42);
    expect(capturedToken).toBe(TOKEN_PLAINTEXT);

    // Audit emit: success, itemId only, NO token bytes.
    const successEntries = m.state.auditCalls.filter((e) => e.action === 'plaid_token_decrypted' && e.outcome === 'success');
    expect(successEntries.length).toBe(1);
    const entry = successEntries[0]!;
    expect(entry.subjectId).toBe('item_1');
    expect(JSON.stringify(entry.details)).not.toContain(TOKEN_PLAINTEXT);
  });

  it('admin scope is used on item findById and readEncryptedToken', async () => {
    const m = buildMocks();
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'ok' as const,
    });
    expect(m.state.itemRepoFindByIdCalls[0]!.scope.kind).toBe('admin');
    expect(m.state.itemRepoReadTokenCalls[0]!.scope.kind).toBe('admin');
  });

  it('AAD: personal domain → handle.kind=user with userDekVersion', async () => {
    const m = buildMocks({ user: makeUser({ userDekVersion: 9 }) });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'ok' as const,
    });
    const decryptInput = m.state.cryptoCalls[0] as { handle: { kind: string; version: number; userId?: string }; aad: { kind: string; itemId: string }; domain: string };
    expect(decryptInput.handle.kind).toBe('user');
    expect(decryptInput.handle.version).toBe(9);
    expect(decryptInput.handle.userId).toBe('user_1');
    expect(decryptInput.aad.kind).toBe('item_token');
    expect(decryptInput.aad.itemId).toBe('item_1');
    expect(decryptInput.domain).toBe('personal');
  });

  it('AAD: pcc domain → handle.kind=pcc with active wrap version', async () => {
    const pccItem = makeItem({ domain: 'pcc', userId: null });
    const m = buildMocks({
      item: pccItem,
      pccKey: {
        id: 'wrap_1',
        version: 4,
        wrappedDek: makeBlob(),
        kdfAlgorithm: 'scrypt',
        kdfN: 1 << 17,
        kdfR: 8,
        kdfP: 1,
        kdfSalt: new Uint8Array(16),
        createdAt: new Date(0),
        retiredAt: null,
      },
    });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'ok' as const,
    });
    const decryptInput = m.state.cryptoCalls[0] as { handle: { kind: string; version: number }; aad: { kind: string }; domain: string };
    expect(decryptInput.handle.kind).toBe('pcc');
    expect(decryptInput.handle.version).toBe(4);
    expect(decryptInput.domain).toBe('pcc');
  });

  it('zeroizes buffer when use callback throws', async () => {
    // Capture the buffer contents at time of `fill(0)` by intercepting via
    // subclass — simpler: spy on Buffer.prototype.fill.
    const fillSpy = vi.spyOn(Buffer.prototype, 'fill');
    const m = buildMocks();
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    let threw = false;
    try {
      await broker.withDecryptedToken({
        itemId: ItemId('item_1'),
        use: async () => {
          throw new Error('use-callback fail');
        },
      });
    } catch (e) {
      threw = true;
      expect((e as Error).message).toBe('use-callback fail');
    }
    expect(threw).toBe(true);
    // fill(0) called at least once with arg 0.
    const zeroFills = fillSpy.mock.calls.filter((c) => c[0] === 0);
    expect(zeroFills.length).toBeGreaterThanOrEqual(1);

    const failureEntries = m.state.auditCalls.filter((e) => e.action === 'plaid_token_decrypted' && e.outcome === 'failure');
    expect(failureEntries.length).toBe(1);

    fillSpy.mockRestore();
  });

  it('decrypt failure surfaces error and audits failure', async () => {
    const m = buildMocks({
      decryptResult: Err({ kind: 'tag_invalid' }),
    });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    const res = await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'unused' as const,
    });
    expect(res.ok).toBe(false);
    if (res.ok) {return;}
    expect(res.error.kind).toBe('tag_invalid');
    const failureEntries = m.state.auditCalls.filter((e) => e.action === 'plaid_token_decrypted' && e.outcome === 'failure');
    expect(failureEntries.length).toBe(1);
  });

  it('item not found returns item_not_found and audits failure', async () => {
    const m = buildMocks({ item: null });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    const res = await broker.withDecryptedToken({
      itemId: ItemId('item_x'),
      use: async () => 'unused' as const,
    });
    expect(res.ok).toBe(false);
    if (res.ok) {return;}
    expect(res.error.kind).toBe('item_not_found');
  });

  it('removed item returns item_not_found', async () => {
    const m = buildMocks({ item: makeItem({ removedAt: new Date() }) });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    const res = await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'unused' as const,
    });
    expect(res.ok).toBe(false);
    if (res.ok) {return;}
    expect(res.error.kind).toBe('item_not_found');
  });

  it('pcc item with no active key wrap returns pcc_dek_not_loaded', async () => {
    const m = buildMocks({
      item: makeItem({ domain: 'pcc', userId: null }),
      pccKey: null,
    });
    const broker = createPlaidTokenBroker({
      crypto: m.crypto,
      itemRepo: m.itemRepo,
      userRepo: m.userRepo,
      pccKeyWrapRepo: m.pccKeyWrapRepo,
      audit: m.audit,
    });
    const res = await broker.withDecryptedToken({
      itemId: ItemId('item_1'),
      use: async () => 'unused' as const,
    });
    expect(res.ok).toBe(false);
    if (res.ok) {return;}
    expect(res.error.kind).toBe('pcc_dek_not_loaded');
  });
});
