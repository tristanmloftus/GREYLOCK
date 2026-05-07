// Greylock — generic empty / no-data / loading copy.
// =============================================================================
// AGENT-UI (Phase 4).
// =============================================================================

import type { ReactNode } from 'react';

import styles from './EmptyState.module.css';

export interface EmptyStateProps {
  readonly title: string;
  readonly description?: string;
  readonly children?: ReactNode;
}

export function EmptyState(props: EmptyStateProps): ReactNode {
  return (
    <div className={styles.wrapper}>
      <div className={styles.title}>{props.title}</div>
      {props.description !== undefined ? (
        <div className={styles.description}>{props.description}</div>
      ) : null}
      {props.children}
    </div>
  );
}
