// Greylock — chain canonical-bytes unit tests
// =============================================================================
// AGENT-AUDIT-LOG. Byte-exact assertions against the canonical layout per
// docs/ARCHITECTURE.md §7. Each fixture below assembles the exact bytes
// `computeEntryHash` should hash, then SHA-256s them independently, and
// compares against the function's output. Equivalence here means every
// downstream change to the canonical layout breaks these tests immediately.
// =============================================================================

import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';

import { ZERO_PREV_HASH, computeEntryHash } from '../../../lib/audit/chain.js';
import {
  ActorKind,
  AuditAction,
  AuditOutcome,
  Domain,
  SubjectKind,
  UserId,
} from '../../../lib/types/domain.js';

// -----------------------------------------------------------------------------
// Reference encoder — re-implements the canonical layout, used as the
// independent oracle. If `computeEntryHash` ever drifts, this oracle will
// disagree and every fixture will fail.
// -----------------------------------------------------------------------------
function refCanonical(input: {
  seq: bigint;
  tsUnixNanos: bigint;
  actorUserId: string | null;
  actorKind: string;
  domain: string | null;
  subjectId: string | null;
  subjectKind: string | null;
  action: string;
  outcome: string;
  detailsJson: string;
  prevHash: Uint8Array;
}): Buffer {
  const utf0 = (s: string): Buffer => {
    const b = Buffer.from(s, 'utf8');
    const out = Buffer.allocUnsafe(b.byteLength + 1);
    b.copy(out, 0);
    out[b.byteLength] = 0;
    return out;
  };
  const u64 = (v: bigint): Buffer => {
    const b = Buffer.allocUnsafe(8);
    b.writeBigUInt64BE(v);
    return b;
  };
  const i64 = (v: bigint): Buffer => {
    const b = Buffer.allocUnsafe(8);
    b.writeBigInt64BE(v);
    return b;
  };
  const u32 = (v: number): Buffer => {
    const b = Buffer.allocUnsafe(4);
    b.writeUInt32BE(v);
    return b;
  };
  const detailsBytes = Buffer.from(input.detailsJson, 'utf8');
  return Buffer.concat([
    u64(input.seq),
    i64(input.tsUnixNanos),
    utf0(input.actorUserId ?? ''),
    utf0(input.actorKind),
    utf0(input.domain ?? ''),
    utf0(input.subjectId ?? ''),
    utf0(input.subjectKind ?? ''),
    utf0(input.action),
    utf0(input.outcome),
    u32(detailsBytes.byteLength),
    detailsBytes,
    Buffer.from(input.prevHash),
  ]);
}

function refHash(input: Parameters<typeof refCanonical>[0]): Uint8Array {
  return new Uint8Array(createHash('sha256').update(refCanonical(input)).digest());
}

// -----------------------------------------------------------------------------
// Fixture vectors
// -----------------------------------------------------------------------------

interface Fixture {
  readonly name: string;
  readonly args: Parameters<typeof refCanonical>[0];
}

const VECTORS: ReadonlyArray<Fixture> = [
  {
    name: 'seq=1, prevHash=zero, empty details',
    args: {
      seq: 1n,
      tsUnixNanos: 0n,
      actorUserId: null,
      actorKind: 'system',
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: 'master_kek_loaded',
      outcome: 'success',
      detailsJson: '',
      prevHash: ZERO_PREV_HASH,
    },
  },
  {
    name: 'seq=2, prevHash=zero, single-byte details',
    args: {
      seq: 2n,
      tsUnixNanos: 1_700_000_000_000_000_000n,
      actorUserId: 'usr_rory',
      actorKind: 'user',
      domain: 'personal',
      subjectId: 'i_a',
      subjectKind: 'item',
      action: 'plaid_link_token_minted',
      outcome: 'success',
      detailsJson: '{}',
      prevHash: ZERO_PREV_HASH,
    },
  },
  {
    name: 'seq=42, all nullable strings empty, larger details',
    args: {
      seq: 42n,
      tsUnixNanos: 1_750_000_000_000_000_000n,
      actorUserId: null,
      actorKind: 'system',
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: 'session_expired',
      outcome: 'success',
      detailsJson: '{"count":17,"reason":"idle_timeout"}',
      prevHash: new Uint8Array(32).fill(0xab),
    },
  },
  {
    name: 'seq=1000, PCC domain, snapshot subject',
    args: {
      seq: 1000n,
      tsUnixNanos: 1_800_000_000_000_000_000n,
      actorUserId: 'usr_tristan',
      actorKind: 'sync_worker',
      domain: 'pcc',
      subjectId: 'sn_pcc_42',
      subjectKind: 'snapshot',
      action: 'net_worth_snapshot_written',
      outcome: 'success',
      detailsJson: '{"netWorthCents":"123456789"}',
      prevHash: new Uint8Array(32).fill(0x7f),
    },
  },
  {
    name: 'seq=2, multibyte unicode in subjectId',
    args: {
      seq: 2n,
      tsUnixNanos: 1n,
      actorUserId: 'usr_x',
      actorKind: 'user',
      domain: 'personal',
      subjectId: 'item-名前-🔑',
      subjectKind: 'item',
      action: 'plaid_token_decrypted',
      outcome: 'success',
      detailsJson: '{"itemId":"i_x"}',
      prevHash: ZERO_PREV_HASH,
    },
  },
  {
    name: 'seq=999999, prevHash all-FF, denied outcome',
    args: {
      seq: 999_999n,
      tsUnixNanos: -1n, // negative ts to exercise the int64 branch
      actorUserId: 'usr_admin',
      actorKind: 'admin_cli',
      domain: null,
      subjectId: 'sess_q',
      subjectKind: 'session',
      action: 'admin_revoke_invoked',
      outcome: 'denied',
      detailsJson: '{"reason":"forbidden"}',
      prevHash: new Uint8Array(32).fill(0xff),
    },
  },
];

describe('computeEntryHash — canonical-bytes vectors', () => {
  for (const v of VECTORS) {
    it(v.name, () => {
      const expected = refHash(v.args);
      const actual = computeEntryHash({
        seq: v.args.seq,
        tsUnixNanos: v.args.tsUnixNanos,
        actorUserId: v.args.actorUserId === null ? null : UserId(v.args.actorUserId),
        actorKind: v.args.actorKind as ActorKind,
        domain: v.args.domain === null ? null : (v.args.domain as Domain),
        subjectId: v.args.subjectId,
        subjectKind: v.args.subjectKind === null ? null : (v.args.subjectKind as SubjectKind),
        action: v.args.action as AuditAction,
        outcome: v.args.outcome as AuditOutcome,
        detailsJson: v.args.detailsJson,
        prevHash: v.args.prevHash,
      });
      expect(Buffer.from(actual).toString('hex')).toBe(Buffer.from(expected).toString('hex'));
    });
  }
});

describe('computeEntryHash — pure + deterministic', () => {
  it('returns the same hash for the same inputs across calls', () => {
    const args = VECTORS[2]!.args;
    const a = computeEntryHash({
      seq: args.seq,
      tsUnixNanos: args.tsUnixNanos,
      actorUserId: null,
      actorKind: args.actorKind as ActorKind,
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: args.action as AuditAction,
      outcome: args.outcome as AuditOutcome,
      detailsJson: args.detailsJson,
      prevHash: args.prevHash,
    });
    const b = computeEntryHash({
      seq: args.seq,
      tsUnixNanos: args.tsUnixNanos,
      actorUserId: null,
      actorKind: args.actorKind as ActorKind,
      domain: null,
      subjectId: null,
      subjectKind: null,
      action: args.action as AuditAction,
      outcome: args.outcome as AuditOutcome,
      detailsJson: args.detailsJson,
      prevHash: args.prevHash,
    });
    expect(Buffer.from(a)).toEqual(Buffer.from(b));
  });
});

describe('computeEntryHash — tamper sensitivity', () => {
  const baseArgs = VECTORS[3]!.args;
  const base = (): Uint8Array =>
    computeEntryHash({
      seq: baseArgs.seq,
      tsUnixNanos: baseArgs.tsUnixNanos,
      actorUserId: baseArgs.actorUserId === null ? null : UserId(baseArgs.actorUserId),
      actorKind: baseArgs.actorKind as ActorKind,
      domain: baseArgs.domain === null ? null : (baseArgs.domain as Domain),
      subjectId: baseArgs.subjectId,
      subjectKind: baseArgs.subjectKind === null ? null : (baseArgs.subjectKind as SubjectKind),
      action: baseArgs.action as AuditAction,
      outcome: baseArgs.outcome as AuditOutcome,
      detailsJson: baseArgs.detailsJson,
      prevHash: baseArgs.prevHash,
    });

  it('changes when seq differs', () => {
    const tampered = computeEntryHash({
      seq: baseArgs.seq + 1n,
      tsUnixNanos: baseArgs.tsUnixNanos,
      actorUserId: baseArgs.actorUserId === null ? null : UserId(baseArgs.actorUserId),
      actorKind: baseArgs.actorKind as ActorKind,
      domain: baseArgs.domain === null ? null : (baseArgs.domain as Domain),
      subjectId: baseArgs.subjectId,
      subjectKind: baseArgs.subjectKind === null ? null : (baseArgs.subjectKind as SubjectKind),
      action: baseArgs.action as AuditAction,
      outcome: baseArgs.outcome as AuditOutcome,
      detailsJson: baseArgs.detailsJson,
      prevHash: baseArgs.prevHash,
    });
    expect(Buffer.from(base())).not.toEqual(Buffer.from(tampered));
  });

  it('changes when prevHash differs by one byte', () => {
    const flipped = new Uint8Array(baseArgs.prevHash);
    flipped[0] = (flipped[0]! ^ 0x01) & 0xff;
    const tampered = computeEntryHash({
      seq: baseArgs.seq,
      tsUnixNanos: baseArgs.tsUnixNanos,
      actorUserId: baseArgs.actorUserId === null ? null : UserId(baseArgs.actorUserId),
      actorKind: baseArgs.actorKind as ActorKind,
      domain: baseArgs.domain === null ? null : (baseArgs.domain as Domain),
      subjectId: baseArgs.subjectId,
      subjectKind: baseArgs.subjectKind === null ? null : (baseArgs.subjectKind as SubjectKind),
      action: baseArgs.action as AuditAction,
      outcome: baseArgs.outcome as AuditOutcome,
      detailsJson: baseArgs.detailsJson,
      prevHash: flipped,
    });
    expect(Buffer.from(base())).not.toEqual(Buffer.from(tampered));
  });

  it('changes when detailsJson differs by a single character', () => {
    const tampered = computeEntryHash({
      seq: baseArgs.seq,
      tsUnixNanos: baseArgs.tsUnixNanos,
      actorUserId: baseArgs.actorUserId === null ? null : UserId(baseArgs.actorUserId),
      actorKind: baseArgs.actorKind as ActorKind,
      domain: baseArgs.domain === null ? null : (baseArgs.domain as Domain),
      subjectId: baseArgs.subjectId,
      subjectKind: baseArgs.subjectKind === null ? null : (baseArgs.subjectKind as SubjectKind),
      action: baseArgs.action as AuditAction,
      outcome: baseArgs.outcome as AuditOutcome,
      detailsJson: baseArgs.detailsJson + ' ',
      prevHash: baseArgs.prevHash,
    });
    expect(Buffer.from(base())).not.toEqual(Buffer.from(tampered));
  });
});

describe('computeEntryHash — guards', () => {
  it('throws on prevHash with wrong length', () => {
    expect(() =>
      computeEntryHash({
        seq: 1n,
        tsUnixNanos: 0n,
        actorUserId: null,
        actorKind: 'system' as ActorKind,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: 'master_kek_loaded' as AuditAction,
        outcome: 'success' as AuditOutcome,
        detailsJson: '',
        prevHash: new Uint8Array(31), // wrong size
      }),
    ).toThrow();
  });
});
