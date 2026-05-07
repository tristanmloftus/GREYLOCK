// Greylock — single domain column (personal or pcc).
// =============================================================================
// AGENT-UI (Phase 4). Composes the column header chip strip + accounts table +
// recent transactions. Re-exported separately from DashboardLayout so the
// admin views can use it standalone if needed.
// =============================================================================

import type { ReactNode } from 'react';

import { AccountTable } from '../account-table/AccountTable';
import type { AccountTableRow } from '../account-table/AccountTable';
import { TransactionTable } from '../transaction-table/TransactionTable';
import type { TransactionTableRow } from '../transaction-table/TransactionTable';
import { EmptyState } from '../empty-state/EmptyState';

import styles from './DashboardLayout.module.css';

export interface DomainColumnProps {
  readonly title: 'Personal' | 'PCC';
  readonly available: boolean;
  readonly netWorthDisplay: string;
  readonly cashDisplay: string;
  readonly monthNetDisplay: string;
  readonly monthNetDirection: 'positive' | 'negative' | 'neutral';
  readonly accounts: ReadonlyArray<AccountTableRow>;
  readonly transactions: ReadonlyArray<TransactionTableRow>;
  readonly emptyDescription?: string;
}

export function DomainColumn(props: DomainColumnProps): ReactNode {
  const dir = props.monthNetDirection;
  const cls = dir === 'positive' ? 'fg-green' : dir === 'negative' ? 'fg-red' : 'fg-muted';
  return (
    <div className={styles.column}>
      <div className={styles.columnHeader}>
        <span className={styles.columnTitle}>{props.title}</span>
        <div className={styles.chips}>
          <span className={styles.chip}>
            <span>NW</span>
            <span className={`${styles.chipValue} num`}>{props.netWorthDisplay}</span>
          </span>
          <span className={styles.chip}>
            <span>CASH</span>
            <span className={`${styles.chipValue} num`}>{props.cashDisplay}</span>
          </span>
          <span className={styles.chip}>
            <span>30D</span>
            <span className={`${styles.chipValue} num ${cls}`}>{props.monthNetDisplay}</span>
          </span>
        </div>
      </div>
      {props.available ? (
        <>
          <AccountTable title={`${props.title} accounts`} rows={props.accounts} />
          <TransactionTable title={`${props.title} recent activity`} rows={props.transactions} />
        </>
      ) : (
        <EmptyState
          title={props.title === 'PCC' ? 'PCC data unavailable' : 'No personal data yet'}
          description={props.emptyDescription ?? 'Connect a bank to populate this column.'}
        />
      )}
    </div>
  );
}
