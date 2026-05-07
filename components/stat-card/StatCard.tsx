// Greylock — single number stat card
// =============================================================================
// AGENT-UI (Phase 4). Pure presentational. The caller formats the cents value
// to a display string (via lib/compute centsToDisplay) — this component does
// not touch numbers itself, only renders.
// =============================================================================

import type { ReactNode } from 'react';

import styles from './StatCard.module.css';

export interface StatCardProps {
  readonly label: string;
  readonly value: string;
  readonly delta?: {
    readonly text: string;
    readonly direction: 'positive' | 'negative' | 'neutral';
  };
  readonly children?: ReactNode;
}

export function StatCard(props: StatCardProps): ReactNode {
  const deltaCls =
    props.delta?.direction === 'positive'
      ? styles.deltaPositive
      : props.delta?.direction === 'negative'
        ? styles.deltaNegative
        : styles.deltaNeutral;
  return (
    <div className={styles.card}>
      <div className={styles.label}>{props.label}</div>
      <div className={`${styles.value} num`}>{props.value}</div>
      {props.delta !== undefined ? (
        <div className={`${styles.delta} ${deltaCls ?? ''} num`}>{props.delta.text}</div>
      ) : null}
      {props.children !== undefined ? <div className={styles.children}>{props.children}</div> : null}
    </div>
  );
}
