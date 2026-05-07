// Greylock — audit-log canonical hash construction
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). PURE bytes-in / bytes-out. No I/O. The single
// source of truth for the hash-chain canonical-bytes layout described in
// docs/ARCHITECTURE.md §7. Byte-exact deviations break
// `pnpm admin:audit-verify` against any chain that already has rows.
//
// Canonical layout — DO NOT change without bumping the version tag in
// ARCHITECTURE.md and migrating existing chains:
//   prevHash    : 32 bytes (zero for seq=1)
//   canonical   : seq          (uint64 BE)
//              || tsUnixNanos  (int64 BE)
//              || actorUserId  (utf8 || 0x00)         // empty string => single 0x00
//              || actorKind    (utf8 || 0x00)
//              || domain       (utf8 || 0x00)         // "" if null
//              || subjectId    (utf8 || 0x00)
//              || subjectKind  (utf8 || 0x00)
//              || action       (utf8 || 0x00)
//              || outcome      (utf8 || 0x00)
//              || len32be(detailsJson) || detailsJson_bytes
//              || prevHash     (32 bytes)
//   entryHash   : SHA-256(canonical)
//
// AGENT-DB's `lib/db/repositories/audit.ts` was the original site of this
// function. Phase 3 lifted it here as the canonical public source per the
// AGENT-AUDIT-LOG brief; the repo now imports from this module.
// =============================================================================

import { createHash } from 'node:crypto';

import type {
  ActorKind,
  AuditAction,
  AuditOutcome,
  Domain,
  SubjectKind,
  UserId,
} from '../types/domain.js';

/** 32 bytes of zeros — the prevHash sentinel for `seq = 1`. */
export const ZERO_PREV_HASH: Uint8Array = new Uint8Array(32);

export interface ComputeEntryHashInput {
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

/**
 * Pure: returns SHA-256(canonical bytes). Deterministic. No side effects.
 * Throws ONLY on programmer error — a negative seq, a detailsJson that
 * doesn't fit in `uint32`, a prevHash that isn't 32 bytes. These are bugs,
 * not runtime errors; the AuditService layer guards inputs before reaching
 * this function.
 */
export function computeEntryHash(input: ComputeEntryHashInput): Uint8Array {
  if (input.prevHash.byteLength !== 32) {
    throw new Error('computeEntryHash: prevHash must be 32 bytes');
  }
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

// -----------------------------------------------------------------------------
// Internal byte-encoders (kept private — callers go through computeEntryHash)
// -----------------------------------------------------------------------------

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
