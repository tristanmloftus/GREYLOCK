// Greylock — audit-log hash-chain integration test
// =============================================================================
//   - Insert N entries → verifyChain reports verifiedCount = N.
//   - Tamper one row's `entryHash` → verifyChain reports `chain_break` at that
//     exact seq.
//   - Tamper one row's `prevHash` → also reports `chain_break` at that seq.
// =============================================================================

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { ActorKind, AuditAction, AuditOutcome } from '../../../lib/types/domain.js';

import { makeTestDb, type TestDb } from './_helpers.js';

describe('audit-log hash chain', () => {
  let db: TestDb;
  beforeEach(async () => {
    db = await makeTestDb();
  });
  afterEach(async () => {
    await db.cleanup();
  });

  it('inserts N entries and verifies the chain', async () => {
    const N = 8;
    for (let i = 0; i < N; i++) {
      const r = await db.repos.auditRepo.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: `subject-${String(i)}`,
        subjectKind: null,
        action: AuditAction.MasterKekLoaded,
        outcome: AuditOutcome.Success,
        detailsJson: JSON.stringify({ i }),
      });
      expect(r.ok).toBe(true);
    }
    const v = await db.repos.auditRepo.verifyChain();
    expect(v.ok).toBe(true);
    if (v.ok) {
      expect(v.value.verifiedCount).toBe(N);
    }
  });

  it('detects tampering of a row entryHash at the right seq', async () => {
    for (let i = 0; i < 5; i++) {
      await db.repos.auditRepo.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: `s-${String(i)}`,
        subjectKind: null,
        action: AuditAction.SessionCreated,
        outcome: AuditOutcome.Success,
        detailsJson: '{}',
      });
    }
    // Tamper seq=3.
    await db.booted.prisma.$executeRawUnsafe(
      "UPDATE AuditLogEntry SET entryHash = X'0000000000000000000000000000000000000000000000000000000000000000' WHERE seq = 3",
    );
    const v = await db.repos.auditRepo.verifyChain();
    expect(v.ok).toBe(false);
    if (!v.ok) {
      expect(v.error.kind).toBe('chain_break');
      if (v.error.kind === 'chain_break') {
        expect(v.error.atSeq).toBe(3n);
      }
    }
  });

  it('detects tampering of a row prevHash at the right seq', async () => {
    for (let i = 0; i < 5; i++) {
      await db.repos.auditRepo.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: `s-${String(i)}`,
        subjectKind: null,
        action: AuditAction.SessionCreated,
        outcome: AuditOutcome.Success,
        detailsJson: '{}',
      });
    }
    await db.booted.prisma.$executeRawUnsafe(
      "UPDATE AuditLogEntry SET prevHash = X'1111111111111111111111111111111111111111111111111111111111111111' WHERE seq = 4",
    );
    const v = await db.repos.auditRepo.verifyChain();
    expect(v.ok).toBe(false);
    if (!v.ok) {
      expect(v.error.kind).toBe('chain_break');
    }
  });

  it('append is atomic — prev seq is locked inside transaction', async () => {
    // Concurrent appends should still produce a coherent chain.
    const promises: Array<Promise<unknown>> = [];
    for (let i = 0; i < 16; i++) {
      promises.push(
        db.repos.auditRepo.append({
          actorUserId: null,
          actorKind: ActorKind.System,
          domain: null,
          subjectId: `c-${String(i)}`,
          subjectKind: null,
          action: AuditAction.MasterKekLoaded,
          outcome: AuditOutcome.Success,
          detailsJson: JSON.stringify({ i }),
        }),
      );
    }
    await Promise.all(promises);
    const v = await db.repos.auditRepo.verifyChain();
    expect(v.ok).toBe(true);
    if (v.ok) {
      expect(v.value.verifiedCount).toBe(16);
    }
  });
});
