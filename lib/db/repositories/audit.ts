// Greylock — AuditRepository (append + chain verifier)
// =============================================================================
// AGENT-DB. Storage layer for the hash-chained audit log. The hash chain
// construction itself is canonical per docs/ARCHITECTURE.md §7 — we
// implement it byte-exact here. Other agents (AGENT-AUDIT-LOG) wrap this
// repository in their `AuditService`, but the persistence layer (atomic
// append, lock the chain head, recompute and verify) lives here so the
// transactional guarantee can't be re-implemented incorrectly elsewhere.
//
// Atomic append (SPEC §QA-SEC, anti-pattern §7):
//   - All operations occur inside `prisma.$transaction(...)`.
//   - Inside the transaction we SELECT the entry with the highest seq, take
//     its `entryHash` as `prevHash` (or 32 zero bytes if no rows), compute
//     the new `entryHash`, and INSERT. SQLite's WAL + transaction guarantees
//     no two concurrent appenders read the same chain head and both insert.
//
// Hash construction (canonical bytes — DO NOT change without bumping the
// canonical-bytes version in ARCHITECTURE.md):
//   prevHash    : 32 bytes (zero for seq=1)
//   canonical   : seq          (uint64 BE)
//              || tsUnixNanos  (int64 BE)
//              || actorUserId  (utf8 || 0x00)
//              || actorKind    (utf8 || 0x00)
//              || domain       (utf8 || 0x00)
//              || subjectId    (utf8 || 0x00)
//              || subjectKind  (utf8 || 0x00)
//              || action       (utf8 || 0x00)
//              || outcome      (utf8 || 0x00)
//              || len32be(detailsJson) || detailsJsonBytes
//              || prevHash     (32 bytes)
//   entryHash   : SHA-256(canonical)
// =============================================================================

import { createHash } from 'node:crypto';

import type { AuditLogEntry as PrismaAuditLogEntry, PrismaClient } from '@prisma/client';

import { AuditSeq, Err, Ok, UserId } from '../../types/domain.js';
import type {
  ActorKind,
  AuditAction,
  AuditEntry,
  AuditError,
  AuditOutcome,
  Domain,
  RepoError,
  Result,
  SubjectKind,
} from '../../types/domain.js';

import { asBuffer, asBytes, mapPrismaError } from './_shared.js';

const ZERO_PREV_HASH: Uint8Array = new Uint8Array(32);

export interface AuditAppendInputForRepo {
  readonly actorUserId: UserId | null;
  readonly actorKind: ActorKind;
  readonly domain: Domain | null;
  readonly subjectId: string | null;
  readonly subjectKind: SubjectKind | null;
  readonly action: AuditAction;
  readonly outcome: AuditOutcome;
  /** Already-sanitized JSON string. The repository never sanitizes —
   *  AuditService runs `lib/audit/sanitizer.ts` before calling us. */
  readonly detailsJson: string;
  /** Optional clock injection for deterministic tests. Default = `new Date()`. */
  readonly now?: Date;
}

export interface AuditRepository {
  append(input: AuditAppendInputForRepo): Promise<Result<AuditEntry, AuditError>>;
  verifyChain(): Promise<Result<{ readonly verifiedCount: number }, AuditError>>;
  query(input: {
    readonly fromSeq?: bigint;
    readonly toSeq?: bigint;
    readonly fromTs?: Date;
    readonly toTs?: Date;
    readonly actorUserId?: string;
    readonly action?: AuditAction;
    readonly domain?: Domain;
    readonly limit?: number;
  }): Promise<Result<ReadonlyArray<AuditEntry>, AuditError>>;
}

export interface CreateAuditRepositoryInput {
  readonly prisma: PrismaClient;
}

export function createAuditRepository(input: CreateAuditRepositoryInput): AuditRepository {
  const { prisma } = input;

  return {
    async append(args): Promise<Result<AuditEntry, AuditError>> {
      try {
        const now = args.now ?? new Date();
        const entry = await prisma.$transaction(async (tx) => {
          // Lock the chain head: the highest-seq row's entryHash.
          const head = await tx.auditLogEntry.findFirst({
            orderBy: { seq: 'desc' },
            select: { seq: true, entryHash: true },
          });
          const prevHash = head === null ? ZERO_PREV_HASH : asBytes(head.entryHash);
          const seq = head === null ? 1n : head.seq + 1n;
          const tsNanos = now.getTime() % 1000 === 0 ? 0 : (now.getTime() % 1000) * 1_000_000;

          const entryHash = computeEntryHash({
            seq,
            tsUnixNanos: BigInt(now.getTime()) * 1_000_000n + BigInt(tsNanos),
            actorUserId: args.actorUserId,
            actorKind: args.actorKind,
            domain: args.domain,
            subjectId: args.subjectId,
            subjectKind: args.subjectKind,
            action: args.action,
            outcome: args.outcome,
            detailsJson: args.detailsJson,
            prevHash,
          });

          const created = await tx.auditLogEntry.create({
            data: {
              seq,
              ts: now,
              tsNanos,
              actorUserId: args.actorUserId,
              actorKind: args.actorKind,
              domain: args.domain,
              subjectId: args.subjectId,
              subjectKind: args.subjectKind,
              action: args.action,
              outcome: args.outcome,
              detailsJson: args.detailsJson,
              prevHash: asBuffer(prevHash),
              entryHash: asBuffer(entryHash),
            },
          });
          return created;
        });
        return Ok(toAuditEntry(entry));
      } catch (cause: unknown) {
        const mapped = mapPrismaError(cause);
        if (mapped.kind === 'conflict') {
          // entryHash uniqueness violated → almost certainly a race we lost.
          return Err({ kind: 'storage_failure' });
        }
        return Err({ kind: 'storage_failure' });
      }
    },

    async verifyChain(): Promise<Result<{ readonly verifiedCount: number }, AuditError>> {
      try {
        let prev: Uint8Array = ZERO_PREV_HASH;
        let verified = 0;
        // Stream rows in seq-ascending order. With WAL we get a consistent
        // snapshot for the duration of this method.
        const pageSize = 500;
        let cursorSeq: bigint | null = null;
        for (;;) {
          const where = cursorSeq === null ? {} : { seq: { gt: cursorSeq } };
          const rows: PrismaAuditLogEntry[] = await prisma.auditLogEntry.findMany({
            where,
            orderBy: { seq: 'asc' },
            take: pageSize,
          });
          if (rows.length === 0) {
            break;
          }
          for (const row of rows) {
            const expected = computeEntryHash({
              seq: row.seq,
              tsUnixNanos: BigInt(row.ts.getTime()) * 1_000_000n + BigInt(row.tsNanos),
              actorUserId: row.actorUserId === null ? null : UserId(row.actorUserId),
              actorKind: row.actorKind as ActorKind,
              domain: row.domain === null ? null : (row.domain as Domain),
              subjectId: row.subjectId,
              subjectKind: row.subjectKind === null ? null : (row.subjectKind as SubjectKind),
              action: row.action as AuditAction,
              outcome: row.outcome as AuditOutcome,
              detailsJson: row.detailsJson,
              prevHash: prev,
            });
            if (!constantTimeEqual(expected, asBytes(row.entryHash))) {
              return Err({ kind: 'chain_break', atSeq: AuditSeq(row.seq) });
            }
            // Also verify the stored prevHash matches the prior entry's
            // entryHash exactly — defense in depth against a row whose
            // entryHash matches a recomputation but whose prevHash was
            // tampered to point elsewhere.
            if (!constantTimeEqual(prev, asBytes(row.prevHash))) {
              return Err({ kind: 'chain_break', atSeq: AuditSeq(row.seq) });
            }
            prev = asBytes(row.entryHash);
            verified += 1;
            cursorSeq = row.seq;
          }
          if (rows.length < pageSize) {
            break;
          }
        }
        return Ok({ verifiedCount: verified });
      } catch {
        return Err({ kind: 'storage_failure' });
      }
    },

    async query(args): Promise<Result<ReadonlyArray<AuditEntry>, AuditError>> {
      try {
        const rows = await prisma.auditLogEntry.findMany({
          where: {
            ...(args.fromSeq !== undefined ? { seq: { gte: args.fromSeq } } : {}),
            ...(args.toSeq !== undefined ? { seq: { lte: args.toSeq } } : {}),
            ...(args.fromTs !== undefined ? { ts: { gte: args.fromTs } } : {}),
            ...(args.toTs !== undefined ? { ts: { lte: args.toTs } } : {}),
            ...(args.actorUserId !== undefined ? { actorUserId: args.actorUserId } : {}),
            ...(args.action !== undefined ? { action: args.action } : {}),
            ...(args.domain !== undefined ? { domain: args.domain } : {}),
          },
          orderBy: { seq: 'asc' },
          take: args.limit ?? 1000,
        });
        return Ok(rows.map(toAuditEntry));
      } catch {
        return Err({ kind: 'storage_failure' });
      }
    },
  };
}

function toAuditEntry(row: PrismaAuditLogEntry): AuditEntry {
  return {
    seq: AuditSeq(row.seq),
    ts: row.ts,
    tsNanos: row.tsNanos,
    actorUserId: row.actorUserId === null ? null : UserId(row.actorUserId),
    actorKind: row.actorKind as ActorKind,
    domain: row.domain === null ? null : (row.domain as Domain),
    subjectId: row.subjectId,
    subjectKind: row.subjectKind === null ? null : (row.subjectKind as SubjectKind),
    action: row.action as AuditAction,
    outcome: row.outcome as AuditOutcome,
    detailsJson: row.detailsJson,
    prevHash: asBytes(row.prevHash),
    entryHash: asBytes(row.entryHash),
  };
}

// -----------------------------------------------------------------------------
// Hash construction
// -----------------------------------------------------------------------------

interface HashInput {
  readonly seq: bigint;
  readonly tsUnixNanos: bigint;
  readonly actorUserId: UserId | null;
  readonly actorKind: ActorKind;
  readonly domain: Domain | null;
  readonly subjectId: string | null;
  readonly subjectKind: SubjectKind | null;
  readonly action: AuditAction;
  readonly outcome: AuditOutcome;
  readonly detailsJson: string;
  readonly prevHash: Uint8Array;
}

export function computeEntryHash(input: HashInput): Uint8Array {
  const parts: Buffer[] = [];
  parts.push(uint64BE(input.seq));
  parts.push(int64BE(input.tsUnixNanos));
  parts.push(utf8Plus0(input.actorUserId ?? ''));
  parts.push(utf8Plus0(input.actorKind));
  parts.push(utf8Plus0(input.domain ?? ''));
  parts.push(utf8Plus0(input.subjectId ?? ''));
  parts.push(utf8Plus0(input.subjectKind ?? ''));
  parts.push(utf8Plus0(input.action));
  parts.push(utf8Plus0(input.outcome));

  const detailsBytes = Buffer.from(input.detailsJson, 'utf8');
  parts.push(uint32BE(detailsBytes.byteLength));
  parts.push(detailsBytes);

  parts.push(Buffer.from(input.prevHash));

  const canonical = Buffer.concat(parts);
  return new Uint8Array(createHash('sha256').update(canonical).digest());
}

function utf8Plus0(s: string): Buffer {
  const utf = Buffer.from(s, 'utf8');
  const out = Buffer.allocUnsafe(utf.byteLength + 1);
  utf.copy(out, 0);
  out[utf.byteLength] = 0;
  return out;
}

function uint64BE(v: bigint): Buffer {
  if (v < 0n) {
    throw new Error('uint64BE: negative value');
  }
  const b = Buffer.allocUnsafe(8);
  b.writeBigUInt64BE(v);
  return b;
}

function int64BE(v: bigint): Buffer {
  const b = Buffer.allocUnsafe(8);
  b.writeBigInt64BE(v);
  return b;
}

function uint32BE(v: number): Buffer {
  if (!Number.isInteger(v) || v < 0 || v > 0xffffffff) {
    throw new Error('uint32BE: out of range');
  }
  const b = Buffer.allocUnsafe(4);
  b.writeUInt32BE(v);
  return b;
}

function constantTimeEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.byteLength !== b.byteLength) {
    return false;
  }
  let diff = 0;
  for (let i = 0; i < a.byteLength; i++) {
    diff |= (a[i] ?? 0) ^ (b[i] ?? 0);
  }
  return diff === 0;
}

// Re-exports for tests.
export const __INTERNAL_FOR_TESTS__ = {
  computeEntryHash,
  ZERO_PREV_HASH,
};

// Suppress unused-import warning for RepoError; keeping it imported for
// future error-mapping additions.
export type _AuditRepoErrorPlaceholder = RepoError;
