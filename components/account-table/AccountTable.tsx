// Greylock — account-level breakdown table.
// =============================================================================
// AGENT-UI (Phase 4). Pure presentational. Caller passes already-sorted rows
// with formatted display strings. Accepting display strings keeps Cents bigint
// out of the browser bundle.
// =============================================================================

import type { ReactNode } from 'react';

import styles from './AccountTable.module.css';

export interface AccountTableRow {
  readonly accountId: string;
  readonly name: string;
  readonly type: string;
  readonly balanceDisplay: string;
  readonly contribution: 'asset' | 'liability';
}

export interface AccountTableProps {
  readonly title: string;
  readonly rows: ReadonlyArray<AccountTableRow>;
}

export function AccountTable(props: AccountTableProps): ReactNode {
  return (
    <section className={styles.wrapper} aria-label={props.title}>
      <div className={styles.heading}>
        <span className={styles.title}>{props.title}</span>
        <span className={styles.count}>{props.rows.length} accounts</span>
      </div>
      {props.rows.length === 0 ? (
        <div className={styles.empty}>No accounts connected.</div>
      ) : (
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">Name</th>
              <th scope="col">Type</th>
              <th scope="col" className={styles.numCell}>
                Balance
              </th>
            </tr>
          </thead>
          <tbody>
            {props.rows.map((r) => {
              const cls = r.contribution === 'liability' ? styles.balanceLiability : styles.balanceAsset;
              return (
                <tr key={r.accountId}>
                  <td>{r.name}</td>
                  <td>
                    <span className={styles.typeTag}>{r.type}</span>
                  </td>
                  <td className={`${styles.numCell} ${cls ?? ''}`}>{r.balanceDisplay}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </section>
  );
}
