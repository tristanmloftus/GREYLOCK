// Greylock — net-worth snapshot writer (post-sync)
// =============================================================================
// AGENT-SYNC (Phase 3). After a successful Plaid `transactions/sync` for an
// item, the orchestrator pulls the latest accounts/transactions and writes a
// per-domain `NetWorthSnapshot` row. The compute functions are pure
// (`lib/compute/*`); we just glue them together with the repos and audit.
// =============================================================================

import { Err, Ok } from '../types/domain.js';
import type {
  Account,
  Cents,
  Domain,
  RepoError,
  Result,
  Transaction,
  UserId,
} from '../types/domain.js';
import {
  ActorKind,
  AuditAction as AuditActionConst,
  AuditOutcome,
} from '../types/domain.js';
import type {
  AccountRepository,
  AuditService,
  ComputeService,
  RepoScope,
  SnapshotRepository,
  TransactionRepository,
} from '../types/services.js';

// -----------------------------------------------------------------------------
// Public types
// -----------------------------------------------------------------------------

export interface SnapshotWriterDeps {
  readonly accountRepo: AccountRepository;
  readonly transactionRepo: TransactionRepository;
  readonly snapshotRepo: SnapshotRepository;
  readonly compute: ComputeService;
  readonly audit: AuditService;
  /** Test seam — defaults to `() => new Date()`. */
  readonly now?: () => Date;
}

export interface WriteSnapshotInput {
  readonly scope: RepoScope;
  readonly domain: Domain;
  /** Personal snapshots have a userId; PCC snapshots are null. */
  readonly userId: UserId | null;
}

export interface WriteSnapshotResult {
  readonly snapshotId: string;
  readonly netWorthCents: Cents;
}

export type SnapshotWriterError =
  | { readonly kind: 'repo_error'; readonly cause: RepoError }
  | { readonly kind: 'compute_failure' };

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

export function createSnapshotWriter(deps: SnapshotWriterDeps): {
  writeSnapshot: (input: WriteSnapshotInput) => Promise<Result<WriteSnapshotResult, SnapshotWriterError>>;
} {
  const now = deps.now ?? ((): Date => new Date());

  async function writeSnapshot(
    input: WriteSnapshotInput,
  ): Promise<Result<WriteSnapshotResult, SnapshotWriterError>> {
    const accountsRes = await deps.accountRepo.listAllInScope(input.scope, {
      domain: input.domain,
    });
    if (!accountsRes.ok) {
      return Err({ kind: 'repo_error', cause: accountsRes.error });
    }
    const accounts: ReadonlyArray<Account> = accountsRes.value;

    // Pull transactions for the rolling 30-day window for monthNet.
    const takenAt = now();
    const fromTs = new Date(takenAt.getTime() - 30 * 24 * 60 * 60 * 1000);
    const txRes = await deps.transactionRepo.listByDateRange(input.scope, {
      fromDate: fromTs,
      toDate: takenAt,
      domain: input.domain,
    });
    const transactions: ReadonlyArray<Transaction> = txRes.ok ? txRes.value : [];

    let netWorthResult;
    let cashCents: Cents;
    let monthNet;
    try {
      netWorthResult = deps.compute.netWorth({ accounts });
      cashCents = deps.compute.cashOnly({ accounts });
      monthNet = deps.compute.monthNet({ transactions, now: takenAt });
    } catch {
      return Err({ kind: 'compute_failure' });
    }

    const inserted = await deps.snapshotRepo.insert(input.scope, {
      domain: input.domain,
      userId: input.userId,
      takenAt,
      assetsCents: netWorthResult.assetsCents,
      liabilitiesCents: netWorthResult.liabilitiesCents,
      netWorthCents: netWorthResult.netWorthCents,
      cashCents,
      monthNetCents: monthNet.netCents,
      computeVersion: 1,
      breakdownJson: JSON.stringify(
        netWorthResult.breakdown.map((b) => ({
          accountId: b.accountId,
          name: b.name,
          type: b.type,
          balanceCents: b.balanceCents.toString(),
          contribution: b.contribution,
        })),
      ),
    });
    if (!inserted.ok) {
      return Err({ kind: 'repo_error', cause: inserted.error });
    }

    try {
      await deps.audit.append({
        actorUserId: input.userId,
        actorKind: ActorKind.SyncWorker,
        domain: input.domain,
        subjectId: inserted.value.id as unknown as string,
        subjectKind: 'snapshot',
        action: AuditActionConst.NetWorthSnapshotWritten,
        outcome: AuditOutcome.Success,
        details: {
          accounts: accounts.length,
          // Cents serialized as strings to avoid bigint-in-JSON issues + to
          // avoid the sanitizer's number-magnitude heuristics.
          netWorthCents: netWorthResult.netWorthCents.toString(),
        },
      });
    } catch {
      // Audit must not block the sync; fall through.
    }

    return Ok({
      snapshotId: inserted.value.id as unknown as string,
      netWorthCents: netWorthResult.netWorthCents,
    });
  }

  return { writeSnapshot };
}
