// Greylock — repository happy-path CRUD tests
// =============================================================================
// Smoke-test every repository method on the encrypted DB. Scope rules are
// proven by the dedicated `scope-by-construction.test.ts` — here we just
// confirm CRUD against a working SQLCipher round-trip.
// =============================================================================

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { Domain, EncryptedBlob, ItemId, UserId } from '../../../lib/types/domain.js';
import type { RepoScope } from '../../../lib/types/services.js';

import { makeTestDb, type TestDb } from './_helpers.js';

describe('repositories — happy path CRUD', () => {
  let db: TestDb;

  beforeEach(async () => {
    db = await makeTestDb();
  });
  afterEach(async () => {
    await db.cleanup();
  });

  // ---------------------------------------------------------------------------
  // UserRepository
  // ---------------------------------------------------------------------------

  it('UserRepository: create / findByEmail / findById / list / setWrappedUserDek', async () => {
    const create = await db.repos.userRepo.create({
      email: 'rory@example.test',
      displayName: 'Rory',
      role: 'owner',
    });
    expect(create.ok).toBe(true);
    if (!create.ok) {throw new Error('unreachable');}
    const userId = create.value.id;

    const byEmail = await db.repos.userRepo.findByEmail('rory@example.test');
    expect(byEmail.ok).toBe(true);
    if (byEmail.ok) {
      expect(byEmail.value?.id).toBe(userId);
    }

    const byId = await db.repos.userRepo.findById(userId);
    expect(byId.ok).toBe(true);

    const list = await db.repos.userRepo.list();
    expect(list.ok).toBe(true);
    if (list.ok) {
      expect(list.value.length).toBe(1);
    }

    const wrapped = EncryptedBlob.unsafeFromBytes(new Uint8Array([1, 2, 3, 4, 5]));
    const set = await db.repos.userRepo.setWrappedUserDek({
      userId,
      version: 1,
      wrapped,
    });
    expect(set.ok).toBe(true);
  });

  it('UserRepository: duplicate email returns conflict', async () => {
    await db.repos.userRepo.create({
      email: 'dup@example.test',
      displayName: 'A',
      role: 'member',
    });
    const second = await db.repos.userRepo.create({
      email: 'dup@example.test',
      displayName: 'B',
      role: 'member',
    });
    expect(second.ok).toBe(false);
    if (!second.ok) {
      expect(second.error.kind).toBe('conflict');
    }
  });

  // ---------------------------------------------------------------------------
  // PasskeyRepository
  // ---------------------------------------------------------------------------

  it('PasskeyRepository: create / findByCredentialId / listByUser / bumpCounter / revoke', async () => {
    const u = await db.repos.userRepo.create({
      email: 'pk@example.test',
      displayName: 'PK',
      role: 'member',
    });
    if (!u.ok) {throw new Error('user setup failed');}

    const credentialId = new Uint8Array([1, 1, 1, 1]);
    const created = await db.repos.passkeyRepo.create({
      userId: u.value.id,
      credentialId,
      credentialPublicKey: new Uint8Array([2, 2, 2, 2]),
      counter: 0n,
      transports: ['internal'],
      aaguid: null,
      deviceLabel: 'rory-laptop',
      kekSalt: new Uint8Array([9, 9, 9, 9]),
    });
    expect(created.ok).toBe(true);
    if (!created.ok) {throw new Error('unreachable');}

    const found = await db.repos.passkeyRepo.findByCredentialId(credentialId);
    expect(found.ok && found.value !== null).toBe(true);

    const list = await db.repos.passkeyRepo.listByUser(u.value.id);
    expect(list.ok && list.value.length === 1).toBe(true);

    const bump = await db.repos.passkeyRepo.bumpCounter({
      id: created.value.id,
      newCounter: 5n,
    });
    expect(bump.ok).toBe(true);

    const revoke = await db.repos.passkeyRepo.revoke({ id: created.value.id });
    expect(revoke.ok).toBe(true);
  });

  // ---------------------------------------------------------------------------
  // SessionRepository
  // ---------------------------------------------------------------------------

  it('SessionRepository: create / find / touch / revoke / expireOverdue', async () => {
    const u = await db.repos.userRepo.create({
      email: 's@example.test',
      displayName: 'S',
      role: 'member',
    });
    if (!u.ok) {throw new Error('user setup failed');}

    const now = new Date();
    const sess = await db.repos.sessionRepo.create({
      userId: u.value.id,
      expiresAt: new Date(now.getTime() + 8 * 3600 * 1000),
      idleTimeoutAt: new Date(now.getTime() + 30 * 60 * 1000),
      userAgent: 'test',
      remoteAddr: '127.0.0.1',
    });
    expect(sess.ok).toBe(true);
    if (!sess.ok) {throw new Error('unreachable');}

    const byId = await db.repos.sessionRepo.findActiveById(sess.value.id);
    expect(byId.ok && byId.value?.id === sess.value.id).toBe(true);

    const byUser = await db.repos.sessionRepo.findActiveByUser(u.value.id);
    expect(byUser.ok && byUser.value?.id === sess.value.id).toBe(true);

    const touch = await db.repos.sessionRepo.touch({
      id: sess.value.id,
      newIdleTimeoutAt: new Date(now.getTime() + 60 * 60 * 1000),
    });
    expect(touch.ok).toBe(true);

    const revoke = await db.repos.sessionRepo.revoke({
      id: sess.value.id,
      reason: 'logout',
    });
    expect(revoke.ok).toBe(true);

    // After revoke, findActiveById is null (status filter).
    const after = await db.repos.sessionRepo.findActiveById(sess.value.id);
    expect(after.ok && after.value === null).toBe(true);

    // expireOverdue: create a new active session with past expiresAt and run.
    const past = await db.repos.sessionRepo.create({
      userId: u.value.id,
      expiresAt: new Date(now.getTime() - 1000),
      idleTimeoutAt: new Date(now.getTime() - 1000),
      userAgent: null,
      remoteAddr: null,
    });
    expect(past.ok).toBe(true);
    const expired = await db.repos.sessionRepo.expireOverdue(now);
    expect(expired.ok && expired.value.count >= 1).toBe(true);
  });

  // ---------------------------------------------------------------------------
  // PccMembershipRepository
  // ---------------------------------------------------------------------------

  it('PccMembershipRepository: add / list / isActiveMember / revoke', async () => {
    const u = await db.repos.userRepo.create({
      email: 'm@example.test',
      displayName: 'M',
      role: 'member',
    });
    if (!u.ok) {throw new Error('user setup failed');}

    const add = await db.repos.pccMembershipRepo.add({ userId: u.value.id });
    expect(add.ok).toBe(true);

    const isMember = await db.repos.pccMembershipRepo.isActiveMember(u.value.id);
    expect(isMember.ok && isMember.value).toBe(true);

    const list = await db.repos.pccMembershipRepo.list();
    expect(list.ok && list.value.length === 1).toBe(true);

    const revoke = await db.repos.pccMembershipRepo.revoke({ userId: u.value.id });
    expect(revoke.ok).toBe(true);

    const after = await db.repos.pccMembershipRepo.isActiveMember(u.value.id);
    expect(after.ok && after.value).toBe(false);
  });

  // ---------------------------------------------------------------------------
  // ItemRepository (with PCC scope)
  // ---------------------------------------------------------------------------

  it('ItemRepository: create / list / readEncryptedToken / softRemove (personal)', async () => {
    const u = await db.repos.userRepo.create({
      email: 'item@example.test',
      displayName: 'I',
      role: 'member',
    });
    if (!u.ok) {throw new Error('user setup failed');}

    const scope: RepoScope = { kind: 'personal', userId: u.value.id };
    const create = await db.repos.itemRepo.create(scope, {
      domain: 'personal',
      userId: u.value.id,
      plaidItemId: 'plaid-item-1' as ReturnType<typeof asUnsafePlaidItemId>,
      plaidInstitutionId: 'ins_1',
      institutionName: 'Test Bank',
      encryptedAccessToken: EncryptedBlob.unsafeFromBytes(new Uint8Array([0xde, 0xad, 0xbe, 0xef])),
    });
    expect(create.ok).toBe(true);
    if (!create.ok) {throw new Error('unreachable');}

    const list = await db.repos.itemRepo.list(scope);
    expect(list.ok && list.value.length === 1).toBe(true);

    const tok = await db.repos.itemRepo.readEncryptedToken(scope, create.value.id);
    expect(tok.ok).toBe(true);
    if (tok.ok) {
      expect(tok.value.byteLength).toBe(4);
    }

    const removed = await db.repos.itemRepo.softRemove(scope, {
      id: create.value.id,
      reason: 'test',
    });
    expect(removed.ok).toBe(true);

    // After soft-remove, readEncryptedToken returns not_found.
    const after = await db.repos.itemRepo.readEncryptedToken(scope, create.value.id);
    expect(after.ok).toBe(false);
    if (!after.ok) {
      expect(after.error.kind).toBe('not_found');
    }
  });

  // ---------------------------------------------------------------------------
  // PccKeyWrapRepository
  // ---------------------------------------------------------------------------

  it('PccKeyWrapRepository: insert / findActive / findByVersion / retire', async () => {
    const wrap = await db.repos.pccKeyWrapRepo.insert({
      version: 1,
      wrappedDek: EncryptedBlob.unsafeFromBytes(new Uint8Array([1, 2, 3])),
      kdfAlgorithm: 'scrypt',
      kdfN: 1 << 17,
      kdfR: 8,
      kdfP: 1,
      kdfSalt: new Uint8Array([7, 7, 7]),
    });
    expect(wrap.ok).toBe(true);

    const active = await db.repos.pccKeyWrapRepo.findActive();
    expect(active.ok && active.value?.version === 1).toBe(true);

    const byVer = await db.repos.pccKeyWrapRepo.findByVersion(1);
    expect(byVer.ok && byVer.value !== null).toBe(true);

    const retire = await db.repos.pccKeyWrapRepo.retire({ version: 1 });
    expect(retire.ok).toBe(true);

    const noActive = await db.repos.pccKeyWrapRepo.findActive();
    expect(noActive.ok && noActive.value === null).toBe(true);
  });

  // ---------------------------------------------------------------------------
  // RateLimitRepository
  // ---------------------------------------------------------------------------

  it('RateLimitRepository: consumeOrTrip — fixed window with cap', async () => {
    const cap = 3;
    const windowMinutes = 15;
    const start = new Date('2026-05-06T12:00:00.000Z');
    const key = 'auth:assert:127.0.0.1:test@example.test';

    for (let i = 1; i <= cap; i++) {
      const r = await db.repos.rateLimitRepo.consumeOrTrip({
        bucketKey: key,
        now: start,
        windowMinutes,
        cap,
      });
      expect(r.ok).toBe(true);
      if (r.ok) {
        expect(r.value.outcome).toBe('consumed');
        expect(r.value.row.count).toBe(i);
      }
    }
    // 4th call within the window → trip.
    const trip = await db.repos.rateLimitRepo.consumeOrTrip({
      bucketKey: key,
      now: start,
      windowMinutes,
      cap,
    });
    expect(trip.ok).toBe(true);
    if (trip.ok) {
      expect(trip.value.outcome).toBe('tripped');
    }
    // After window rolls over → next call resets to 1.
    const after = new Date(start.getTime() + (windowMinutes + 1) * 60_000);
    const reset = await db.repos.rateLimitRepo.consumeOrTrip({
      bucketKey: key,
      now: after,
      windowMinutes,
      cap,
    });
    expect(reset.ok).toBe(true);
    if (reset.ok) {
      expect(reset.value.outcome).toBe('consumed');
      expect(reset.value.row.count).toBe(1);
    }
  });

  // ---------------------------------------------------------------------------
  // EnrollmentTokenRepository
  // ---------------------------------------------------------------------------

  it('EnrollmentTokenRepository: mint / verify / consume one-shot', async () => {
    const { mintEnrollmentToken } =
      await import('../../../lib/db/repositories/enrollment-token.js');
    const minted = await mintEnrollmentToken({
      prisma: db.booted.prisma,
      email: 'rory@example.test',
      ttlMinutes: 30,
    });
    expect(minted.ok).toBe(true);
    if (!minted.ok) {throw new Error('mint failed');}

    const v1 = await db.repos.enrollmentTokenRepo.verify({
      token: minted.value.cleartextToken,
    });
    expect(v1.ok).toBe(true);
    if (v1.ok) {
      expect(v1.value.email).toBe('rory@example.test');
    }

    const c1 = await db.repos.enrollmentTokenRepo.consume({ tokenId: minted.value.tokenId });
    expect(c1.ok).toBe(true);

    // Second consume must fail with `token_already_used`.
    const c2 = await db.repos.enrollmentTokenRepo.consume({ tokenId: minted.value.tokenId });
    expect(c2.ok).toBe(false);
    if (!c2.ok) {
      expect(c2.error.kind).toBe('token_already_used');
    }

    // verify on a used token now returns `token_already_used`.
    const v2 = await db.repos.enrollmentTokenRepo.verify({
      token: minted.value.cleartextToken,
    });
    expect(v2.ok).toBe(false);
    if (!v2.ok) {
      expect(v2.error.kind).toBe('token_already_used');
    }
  });

  it('EnrollmentTokenRepository: bogus token returns token_not_found', async () => {
    const r = await db.repos.enrollmentTokenRepo.verify({ token: 'aaaaaaaa' });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('token_not_found');
    }
  });

  // Suppress unused-var hint
  void Domain.Personal;
  void ItemId;
  void UserId;
});

// Tiny local cast that mirrors what live code does at the boundary.
// eslint-disable-next-line @typescript-eslint/no-unused-vars -- used as a `typeof` source for branded casts above
const asUnsafePlaidItemId = (s: string): import('../../../lib/types/domain.js').PlaidItemId =>
  s as import('../../../lib/types/domain.js').PlaidItemId;
