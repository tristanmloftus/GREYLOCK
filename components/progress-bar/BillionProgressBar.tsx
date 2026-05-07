// Greylock — $1B progress bar
// =============================================================================
// AGENT-UI (Phase 4). Pure CSS bar with milestone tick marks at $100K, $10M,
// $100M and a final $1B label. Caller passes the progress fraction as a number
// in [0, 1] (already clamped by lib/compute/billion-progress).
//
// We render ticks on a logarithmic axis so the early milestones don't cluster
// at the very left edge. Specifically, we distribute milestones evenly across
// the bar but do NOT distort the *fill* — the fill is linear progress (the
// real value), the labels are reference markers.
// =============================================================================

import type { ReactNode, CSSProperties } from 'react';

import styles from './BillionProgressBar.module.css';

export interface BillionProgressBarProps {
  /** Number in [0, 1]. */
  readonly progress: number;
}

const MILESTONES = ['$100K', '$10M', '$100M', '$1B'];

export function BillionProgressBar(props: BillionProgressBarProps): ReactNode {
  const clamped = Math.min(1, Math.max(0, props.progress));
  const pct = `${(clamped * 100).toFixed(2)}%`;
  // Inline custom property — used only by the .fill rule. CSP-safe (it's a
  // declared custom property, not an inline style/script execution).
  const style = { ['--fill' as string]: pct } as CSSProperties;
  return (
    <div className={styles.wrapper} role="progressbar" aria-valuemin={0} aria-valuemax={1} aria-valuenow={clamped}>
      <div className={styles.track}>
        <div className={styles.fill} style={style} />
      </div>
      <div className={styles.ticks} aria-hidden="true">
        {[25, 50, 75].map((p) => (
          <span
            key={p}
            className={styles.tick}
            style={{ left: `${p}%` }}
          />
        ))}
      </div>
      <div className={styles.labels} aria-hidden="true">
        {MILESTONES.map((m) => (
          <span key={m}>{m}</span>
        ))}
      </div>
    </div>
  );
}
