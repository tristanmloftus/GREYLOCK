// Greylock — AuditService implementation
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). Wraps the `AuditRepository` produced by
// AGENT-DB. Three responsibilities:
//   1. `append`  — sanitize details via `sanitizer.sanitizeDetails`. Reject
//                   surfaces as `AuditError {kind: 'sanitizer_rejected_payload'}`.
//                   Repository never sees unsanitized data.
//   2. `query`   — pass-through. Limit hard-capped at the route layer; we
//                   forward exactly what we're given here.
//   3. `verifyChain` — pass-through.
//
// The repo's `append` takes a JSON-string `detailsJson`; we serialize the
// sanitized object once and hand it down. Serialization can fail (e.g. a
// circular ref the sanitizer didn't catch — defensive), in which case we
// also surface `sanitizer_rejected_payload`.
// =============================================================================

import { Err, Ok } from '../types/domain.js';
import type { AuditEntry, AuditError, Result } from '../types/domain.js';
import type {
  AuditAppendInput,
  AuditQueryInput,
  AuditService,
} from '../types/services.js';
import type { AuditRepository } from '../db/repositories/audit.js';

import { sanitizeDetails } from './sanitizer.js';

export interface AuditServiceDeps {
  readonly auditRepo: AuditRepository;
}

/** Construct the canonical AuditService implementation. The factory takes
 *  the repository as a dependency so tests can inject an in-memory or
 *  fake repo. */
export function createAuditService(deps: AuditServiceDeps): AuditService {
  const repo = deps.auditRepo;

  return {
    async append(input: AuditAppendInput): Promise<Result<AuditEntry, AuditError>> {
      const sanitized = sanitizeDetails(input.details as Record<string, unknown>);
      if (!sanitized.ok) {
        return Err({ kind: 'sanitizer_rejected_payload' });
      }
      let detailsJson: string;
      try {
        detailsJson = JSON.stringify(sanitized.sanitized);
      } catch {
        return Err({ kind: 'sanitizer_rejected_payload' });
      }
      return repo.append({
        actorUserId: input.actorUserId,
        actorKind: input.actorKind,
        domain: input.domain,
        subjectId: input.subjectId,
        subjectKind: input.subjectKind,
        action: input.action,
        outcome: input.outcome,
        detailsJson,
      });
    },

    async query(input: AuditQueryInput): Promise<Result<ReadonlyArray<AuditEntry>, AuditError>> {
      // Pass-through. Route handler enforces the 1000-row cap; if a caller
      // bypasses the route, the repo's default-1000 still applies.
      return repo.query({
        ...(input.fromSeq !== undefined ? { fromSeq: input.fromSeq } : {}),
        ...(input.toSeq !== undefined ? { toSeq: input.toSeq } : {}),
        ...(input.fromTs !== undefined ? { fromTs: input.fromTs } : {}),
        ...(input.toTs !== undefined ? { toTs: input.toTs } : {}),
        ...(input.actorUserId !== undefined ? { actorUserId: input.actorUserId } : {}),
        ...(input.action !== undefined ? { action: input.action } : {}),
        ...(input.domain !== undefined ? { domain: input.domain } : {}),
        ...(input.limit !== undefined ? { limit: input.limit } : {}),
      });
    },

    async verifyChain(): Promise<Result<{ readonly verifiedCount: number }, AuditError>> {
      return repo.verifyChain();
    },
  };
}

// Suppress unused-symbol lints for `Ok` (we use Err only on the local
// rejection paths; Ok flows through the repo's Result).
const _unused = { Ok };
void _unused;
