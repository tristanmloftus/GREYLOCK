// Greylock — dashboard data fetchers (server-only).
// =============================================================================
// AGENT-UI (Phase 4). Pulls the latest NetWorthSnapshot, accounts, and recent
// transactions for a single (domain, scope) — formatted into UI-ready strings
// using lib/compute/currency. Cents NEVER leak into the browser; the React
// tree only ever sees display strings.
// =============================================================================

import { centsToDisplay } from '../../lib/compute';
import { getFullRepos } from '../../lib/runtime/services-registry';
import type { Cents, NetWorthSnapshot } from '../../lib/types/domain';
import type { Domain, UserId } from '../../lib/types/domain';
import type { RepoScope, SnapshotRepository } from '../../lib/types/services';

import type { AccountTableRow } from '../../components/account-table/AccountTable';
import type { TransactionTableRow } from '../../components/transaction-table/TransactionTable';
import type { DashboardColumn } from '../../components/dashboard/DashboardLayout';

const TX_WINDOW_DAYS = 30;
const TX_DEFAULT_LIMIT = 20;
const GOAL_CENTS = 100_000_000_000n; // $1B in cents

export interface DashboardData {
  readonly summary: {
    readonly netWorthDisplay: string;
    readonly cashDisplay: string;
    readonly monthNetDisplay: string;
    readonly billionDisplay: string;
    readonly billionProgress: number;
    readonly netWorthDelta?: { readonly text: string; readonly direction: 'positive' | 'negative' | 'neutral' };
    readonly monthNetDelta?: { readonly text: string; readonly direction: 'positive' | 'negative' | 'neutral' };
  };
  readonly personal: DashboardColumn;
  readonly pcc: DashboardColumn;
  readonly lastSyncAt: Date | null;
}

interface ColumnInputs {
  readonly available: boolean;
  readonly netWorthCents: Cents;
  readonly cashCents: Cents;
  readonly monthNetCents: Cents | null;
  readonly accounts: ReadonlyArray<AccountTableRow>;
  readonly transactions: ReadonlyArray<TransactionTableRow>;
  readonly lastSyncAt: Date | null;
}

function dirOf(c: Cents): 'positive' | 'negative' | 'neutral' {
  if (c > 0n) {
    return 'positive';
  }
  if (c < 0n) {
    return 'negative';
  }
  return 'neutral';
}

function formatTxDate(d: Date): string {
  const yyyy = d.getFullYear();
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  return `${yyyy}-${mm}-${dd}`;
}

async function fetchColumnData(
  scope: RepoScope,
  domain: Domain,
  userIdForSnapshot: UserId | null,
  now: Date,
): Promise<ColumnInputs> {
  let repos;
  try {
    repos = await getFullRepos();
  } catch {
    return {
      available: false,
      netWorthCents: 0n as Cents,
      cashCents: 0n as Cents,
      monthNetCents: null,
      accounts: [],
      transactions: [],
      lastSyncAt: null,
    };
  }

  // Snapshot (or zero/empty if no snapshot persisted yet).
  let netWorthCents: Cents = 0n as Cents;
  let cashCents: Cents = 0n as Cents;
  let monthNetCents: Cents | null = null;
  let lastSyncAt: Date | null = null;
  // The snapshot repo is part of the booted-DB bundle but not exposed via
  // getFullRepos. We reach for it via the booted-DB module directly.
  type DbModShape = {
    readonly getBootedDb?: () => {
      readonly repos: {
        readonly snapshotRepo: SnapshotRepository;
      };
    };
  };
  let snapshotRes:
    | { readonly ok: true; readonly value: NetWorthSnapshot | null }
    | { readonly ok: false } = { ok: false };
  try {
    const dbPath = '../../lib/db/index.js';
    const dbMod = (await import(/* @vite-ignore */ dbPath)) as DbModShape;
    if (dbMod.getBootedDb !== undefined) {
      const snapshotRepo = dbMod.getBootedDb().repos.snapshotRepo;
      const r = await snapshotRepo.latest(scope, { domain, userId: userIdForSnapshot });
      if (r.ok) {
        snapshotRes = { ok: true, value: r.value };
      }
    }
  } catch {
    // fall through; snapshotRes stays unavailable
  }
  if (snapshotRes.ok && snapshotRes.value !== null) {
    netWorthCents = snapshotRes.value.netWorthCents;
    cashCents = snapshotRes.value.cashCents;
    monthNetCents = snapshotRes.value.monthNetCents;
    lastSyncAt = snapshotRes.value.takenAt;
  }

  // Account list (across items).
  const accountsRes = await repos.accountRepo.listAllInScope(scope, { domain });
  const accounts: AccountTableRow[] = [];
  let computedCash = 0n;
  let computedNet = 0n;
  if (accountsRes.ok) {
    for (const a of accountsRes.value) {
      const balance: Cents = (a.currentBalanceCents ?? (0n as Cents));
      const isLiability = a.type === 'credit' || a.type === 'loan';
      const contribution: 'asset' | 'liability' = isLiability ? 'liability' : 'asset';
      if (a.type === 'depository' && balance > 0n) {
        computedCash += balance;
      }
      computedNet += isLiability ? -balance : balance;
      accounts.push({
        accountId: a.id,
        name: a.name,
        type: a.type,
        balanceDisplay: centsToDisplay(balance),
        contribution,
      });
    }
  }
  // If no snapshot yet, fall back to computed values from current accounts.
  if (!snapshotRes.ok || snapshotRes.value === null) {
    if (accountsRes.ok && accountsRes.value.length > 0) {
      netWorthCents = computedNet as Cents;
      cashCents = computedCash as Cents;
    }
  }

  // Recent transactions.
  const fromDate = new Date(now.getTime() - TX_WINDOW_DAYS * 24 * 60 * 60 * 1000);
  const txRes = await repos.transactionRepo.listByDateRange(scope, {
    fromDate,
    toDate: now,
    domain,
  });
  const transactions: TransactionTableRow[] = [];
  if (txRes.ok) {
    const sorted = [...txRes.value]
      .sort((a, b) => b.date.getTime() - a.date.getTime())
      .slice(0, TX_DEFAULT_LIMIT);
    for (const t of sorted) {
      // Plaid sign convention: + = outflow, - = inflow. Convert to display.
      const isOutflow = t.amountCents > 0n;
      transactions.push({
        transactionId: t.id,
        date: formatTxDate(t.date),
        name: t.name,
        category: t.category,
        amountDisplay: centsToDisplay(t.amountCents, { sign: 'always' }),
        direction: isOutflow ? 'outflow' : 'inflow',
      });
    }
  }

  const available = accountsRes.ok && accountsRes.value.length > 0;
  return {
    available,
    netWorthCents,
    cashCents,
    monthNetCents,
    accounts,
    transactions,
    lastSyncAt,
  };
}

export interface FetchDashboardArgs {
  readonly userId: UserId;
  readonly isPccMember: boolean;
  readonly now?: Date;
}

export async function fetchDashboardData(args: FetchDashboardArgs): Promise<DashboardData> {
  const now = args.now ?? new Date();

  // Personal column — always for `userId`.
  const personalScope: RepoScope = { kind: 'personal', userId: args.userId };
  const personal = await fetchColumnData(personalScope, 'personal', args.userId, now);

  // PCC column — only if member.
  let pcc: ColumnInputs;
  if (args.isPccMember) {
    const pccScope: RepoScope = { kind: 'pcc', memberOfUserId: args.userId };
    pcc = await fetchColumnData(pccScope, 'pcc', null, now);
  } else {
    pcc = {
      available: false,
      netWorthCents: 0n as Cents,
      cashCents: 0n as Cents,
      monthNetCents: null,
      accounts: [],
      transactions: [],
      lastSyncAt: null,
    };
  }

  // Combined summary across visible domains.
  const totalNet = personal.netWorthCents + pcc.netWorthCents;
  const totalCash = personal.cashCents + pcc.cashCents;
  const totalMonthNet =
    (personal.monthNetCents ?? (0n as Cents)) + (pcc.monthNetCents ?? (0n as Cents));
  const billionRatio = Number(totalNet) / Number(GOAL_CENTS);
  const billionProgress = Math.max(0, Math.min(1, billionRatio));
  const billionDisplay = `${(billionProgress * 100).toFixed(2)}%`;

  // Pick the freshest sync timestamp across columns.
  let lastSyncAt: Date | null = null;
  for (const ts of [personal.lastSyncAt, pcc.lastSyncAt]) {
    if (ts === null) {
      continue;
    }
    if (lastSyncAt === null || ts.getTime() > lastSyncAt.getTime()) {
      lastSyncAt = ts;
    }
  }

  return {
    summary: {
      netWorthDisplay: centsToDisplay(totalNet as Cents),
      cashDisplay: centsToDisplay(totalCash as Cents),
      monthNetDisplay: centsToDisplay(totalMonthNet as Cents, { sign: 'always' }),
      billionDisplay,
      billionProgress,
      monthNetDelta: {
        text: '30d window',
        direction: dirOf(totalMonthNet as Cents),
      },
    },
    personal: {
      title: 'Personal',
      available: personal.available,
      netWorthDisplay: centsToDisplay(personal.netWorthCents),
      cashDisplay: centsToDisplay(personal.cashCents),
      monthNetDisplay: centsToDisplay((personal.monthNetCents ?? (0n as Cents)) as Cents, { sign: 'always' }),
      monthNetDirection: dirOf((personal.monthNetCents ?? (0n as Cents)) as Cents),
      accounts: personal.accounts,
      transactions: personal.transactions,
      emptyDescription: 'Connect a personal bank to populate this column.',
    },
    pcc: {
      title: 'PCC',
      available: args.isPccMember && pcc.available,
      netWorthDisplay: centsToDisplay(pcc.netWorthCents),
      cashDisplay: centsToDisplay(pcc.cashCents),
      monthNetDisplay: centsToDisplay((pcc.monthNetCents ?? (0n as Cents)) as Cents, { sign: 'always' }),
      monthNetDirection: dirOf((pcc.monthNetCents ?? (0n as Cents)) as Cents),
      accounts: pcc.accounts,
      transactions: pcc.transactions,
      emptyDescription: args.isPccMember
        ? 'No PCC items connected yet.'
        : 'PCC member required.',
    },
    lastSyncAt,
  };
}
