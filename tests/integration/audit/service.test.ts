// Greylock — AuditService integration test
// =============================================================================
// AGENT-AUDIT-LOG. Wires the service against a real encrypted SQLCipher DB
// + the real `AuditRepository`. Asserts:
//   - happy-path append + query + verifyChain
//   - sanitizer rejection surfaces as `AuditError` (no row written)
//   - tampered row triggers chain_break at the correct seq
// =============================================================================

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { createAuditService } from '../../../lib/audit/index.js';
import {
  ActorKind,
  AuditAction,
  AuditOutcome,
  Domain,
} from '../../../lib/types/domain.js';

import { makeTestDb, type TestDb } from '../db/_helpers.js';

describe('AuditService — integration against real AuditRepository', () => {
  let db: TestDb;

  beforeEach(async () => {
    db = await makeTestDb();
  });
  afterEach(async () => {
    await db.cleanup();
  });

  it('append + query + verifyChain happy path', async () => {
    const service = createAuditService({ auditRepo: db.repos.auditRepo });

    const N = 6;
    for (let i = 0; i < N; i++) {
      const r = await service.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: `s-${String(i)}`,
        subjectKind: null,
        action: AuditAction.MasterKekLoaded,
        outcome: AuditOutcome.Success,
        details: { count: i, reason: 'boot' },
      });
      expect(r.ok).toBe(true);
    }

    const all = await service.query({});
    expect(all.ok).toBe(true);
    if (all.ok) {
      expect(all.value.length).toBe(N);
      // Sanitized payload reaches storage unchanged for clean inputs.
      expect(JSON.parse(all.value[0]!.detailsJson)).toEqual({ count: 0, reason: 'boot' });
    }

    const verified = await service.verifyChain();
    expect(verified.ok).toBe(true);
    if (verified.ok) {
      expect(verified.value.verifiedCount).toBe(N);
    }
  });

  it('sanitizer rejection surfaces as AuditError without writing the row', async () => {
    const service = createAuditService({ auditRepo: db.repos.auditRepo });

    const before = await service.query({});
    expect(before.ok).toBe(true);
    const beforeCount = before.ok ? before.value.length : -1;

    const r = await service.append({
      actorUserId: null,
      actorKind: ActorKind.System,
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: AuditAction.MasterKekLoaded,
      outcome: AuditOutcome.Success,
      // 'password' is on the deny-list — must reject.
      details: { password: 'leaked' },
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('sanitizer_rejected_payload');
    }

    // No row inserted.
    const after = await service.query({});
    expect(after.ok).toBe(true);
    if (after.ok) {
      expect(after.value.length).toBe(beforeCount);
    }

    // Chain still verifies cleanly because we wrote nothing.
    const verified = await service.verifyChain();
    expect(verified.ok).toBe(true);
  });

  it('also rejects token-shape values', async () => {
    const service = createAuditService({ auditRepo: db.repos.auditRepo });
    const r = await service.append({
      actorUserId: null,
      actorKind: ActorKind.System,
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: AuditAction.PlaidTokenDecrypted,
      outcome: AuditOutcome.Success,
      // Token-shape value at allowlisted key still rejects.
      details: { reason: 'a'.repeat(64) },
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('sanitizer_rejected_payload');
    }
  });

  it('chain verify catches a manual tamper via raw UPDATE', async () => {
    const service = createAuditService({ auditRepo: db.repos.auditRepo });

    for (let i = 0; i < 5; i++) {
      const r = await service.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: `s-${String(i)}`,
        subjectKind: null,
        action: AuditAction.SessionCreated,
        outcome: AuditOutcome.Success,
        details: {},
      });
      expect(r.ok).toBe(true);
    }

    // Tamper seq=3's entryHash.
    await db.booted.prisma.$executeRawUnsafe(
      "UPDATE AuditLogEntry SET entryHash = X'0000000000000000000000000000000000000000000000000000000000000000' WHERE seq = 3",
    );

    const v = await service.verifyChain();
    expect(v.ok).toBe(false);
    if (!v.ok) {
      expect(v.error.kind).toBe('chain_break');
      if (v.error.kind === 'chain_break') {
        expect(v.error.atSeq).toBe(3n);
      }
    }
  });

  it('query honors filters', async () => {
    const service = createAuditService({ auditRepo: db.repos.auditRepo });
    await service.append({
      actorUserId: null,
      actorKind: ActorKind.System,
      domain: Domain.Personal,
      subjectId: 's1',
      subjectKind: null,
      action: AuditAction.PasskeyEnrollment,
      outcome: AuditOutcome.Success,
      details: {},
    });
    await service.append({
      actorUserId: null,
      actorKind: ActorKind.System,
      domain: Domain.Pcc,
      subjectId: 's2',
      subjectKind: null,
      action: AuditAction.PccDekUnwrapped,
      outcome: AuditOutcome.Success,
      details: {},
    });

    const personal = await service.query({ domain: Domain.Personal });
    expect(personal.ok).toBe(true);
    if (personal.ok) {
      expect(personal.value.every((e) => e.domain === Domain.Personal)).toBe(true);
    }

    const pcc = await service.query({ domain: Domain.Pcc });
    expect(pcc.ok).toBe(true);
    if (pcc.ok) {
      expect(pcc.value.every((e) => e.domain === Domain.Pcc)).toBe(true);
    }
  });
});
