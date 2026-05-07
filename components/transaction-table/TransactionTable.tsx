// Greylock — recent transactions table.
// =============================================================================
// AGENT-UI (Phase 4). Pure presentational. Caller pre-formats date and amount
// display strings (so the browser never sees bigint Cents).
// =============================================================================

import type { ReactNode } from 'react';

import styles from './TransactionTable.module.css';

export interface TransactionTableRow {
  readonly transactionId: string;
  readonly date: string;
  readonly name: string;
  readonly category: string | null;
  readonly amountDisplay: string;
  /** 'inflow' = money in (display green), 'outflow' = money out (display red). */
  readonly direction: 'inflow' | 'outflow';
}

export interface TransactionTableProps {
  readonly title: string;
  readonly rows: ReadonlyArray<TransactionTableRow>;
}

export function TransactionTable(props: TransactionTableProps): ReactNode {
  return (
    <section className={styles.wrapper} aria-label={props.title}>
      <div className={styles.heading}>
        <span className={styles.title}>{props.title}</span>
        <span className={styles.count}>{props.rows.length} entries</span>
      </div>
      {props.rows.length === 0 ? (
        <div className={styles.empty}>No transactions yet.</div>
      ) : (
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">Date</th>
              <th scope="col">Name</th>
              <th scope="col">Category</th>
              <th scope="col" className={styles.numCell}>
                Amount
              </th>
            </tr>
          </thead>
          <tbody>
            {props.rows.map((r) => {
              const cls = r.direction === 'inflow' ? styles.amountIn : styles.amountOut;
              return (
                <tr key={r.transactionId}>
                  <td className={styles.dateCell}>{r.date}</td>
                  <td>{r.name}</td>
                  <td>{r.category ?? '—'}</td>
                  <td className={`${styles.numCell} ${cls ?? ''}`}>{r.amountDisplay}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </section>
  );
}
