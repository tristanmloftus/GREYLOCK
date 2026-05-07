// Greylock — dashboard layout
// =============================================================================
// AGENT-UI (Phase 4). Server component that composes the four-metric strip,
// the two-column personal/PCC split, and (optionally) a 30s polling client
// component. It accepts already-formatted display strings — the page server
// component is responsible for translating Cents -> display via lib/compute.
// =============================================================================

import type { ReactNode } from 'react';

import { StatCard } from '../stat-card/StatCard';
import { BillionProgressBar } from '../progress-bar/BillionProgressBar';
import { AccountTable } from '../account-table/AccountTable';
import type { AccountTableRow } from '../account-table/AccountTable';
import { TransactionTable } from '../transaction-table/TransactionTable';
import type { TransactionTableRow } from '../transaction-table/TransactionTable';
import { EmptyState } from '../empty-state/EmptyState';

import styles from './DashboardLayout.module.css';

export interface DashboardSummary {
  readonly netWorthDisplay: string;
  readonly netWorthDelta?: { readonly text: string; readonly direction: 'positive' | 'negative' | 'neutral' };
  readonly cashDisplay: string;
  readonly monthNetDisplay: string;
  readonly monthNetDelta?: { readonly text: string; readonly direction: 'positive' | 'negative' | 'neutral' };
  readonly billionProgress: number;
  readonly billionDisplay: string;
}

export interface DashboardColumn {
  readonly title: 'Personal' | 'PCC';
  /** If null, the column shows an empty / not-available state. */
  readonly available: boolean;
  readonly netWorthDisplay: string;
  readonly cashDisplay: string;
  readonly monthNetDisplay: string;
  readonly monthNetDirection: 'positive' | 'negative' | 'neutral';
  readonly accounts: ReadonlyArray<AccountTableRow>;
  readonly transactions: ReadonlyArray<TransactionTableRow>;
  readonly emptyDescription?: string;
}

export interface DashboardLayoutProps {
  readonly summary: DashboardSummary;
  readonly personal: DashboardColumn;
  readonly pcc: DashboardColumn;
}

function Chips(props: { readonly column: DashboardColumn }): ReactNode {
  const dir = props.column.monthNetDirection;
  const cls = dir === 'positive' ? 'fg-green' : dir === 'negative' ? 'fg-red' : 'fg-muted';
  return (
    <div className={styles.chips}>
      <span className={styles.chip}>
        <span>NW</span>
        <span className={`${styles.chipValue} num`}>{props.column.netWorthDisplay}</span>
      </span>
      <span className={styles.chip}>
        <span>CASH</span>
        <span className={`${styles.chipValue} num`}>{props.column.cashDisplay}</span>
      </span>
      <span className={styles.chip}>
        <span>30D</span>
        <span className={`${styles.chipValue} num ${cls}`}>{props.column.monthNetDisplay}</span>
      </span>
    </div>
  );
}

function Column(props: { readonly column: DashboardColumn }): ReactNode {
  const c = props.column;
  return (
    <div className={styles.column}>
      <div className={styles.columnHeader}>
        <span className={styles.columnTitle}>{c.title}</span>
        <Chips column={c} />
      </div>
      {c.available ? (
        <>
          <AccountTable title={`${c.title} accounts`} rows={c.accounts} />
          <TransactionTable title={`${c.title} recent activity`} rows={c.transactions} />
        </>
      ) : (
        <EmptyState
          title={c.title === 'PCC' ? 'PCC data unavailable' : 'No personal data yet'}
          description={c.emptyDescription ?? 'Connect a bank to populate this column.'}
        />
      )}
    </div>
  );
}

export function DashboardLayout(props: DashboardLayoutProps): ReactNode {
  const s = props.summary;
  return (
    <div className={styles.layout}>
      <div className={styles.summaryStrip}>
        <StatCard
          label="Net Worth"
          value={s.netWorthDisplay}
          {...(s.netWorthDelta !== undefined ? { delta: s.netWorthDelta } : {})}
        />
        <StatCard label="$1B Progress" value={s.billionDisplay}>
          <BillionProgressBar progress={s.billionProgress} />
        </StatCard>
        <StatCard label="Cash" value={s.cashDisplay} />
        <StatCard
          label="Month Net (30d)"
          value={s.monthNetDisplay}
          {...(s.monthNetDelta !== undefined ? { delta: s.monthNetDelta } : {})}
        />
      </div>
      <div className={styles.split}>
        <Column column={props.personal} />
        <Column column={props.pcc} />
      </div>
    </div>
  );
}
