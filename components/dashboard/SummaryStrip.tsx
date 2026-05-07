// Greylock — four-metric strip wrapper
// =============================================================================
// AGENT-UI (Phase 4). Thin re-export so the brief's named component (`<SummaryStrip>`)
// resolves. The actual layout is composed inside DashboardLayout.tsx; this is
// the stand-alone version for tests and storybook-style isolation.
// =============================================================================

import type { ReactNode } from 'react';

import { StatCard } from '../stat-card/StatCard';
import { BillionProgressBar } from '../progress-bar/BillionProgressBar';

import styles from './DashboardLayout.module.css';

export interface SummaryStripProps {
  readonly netWorthDisplay: string;
  readonly cashDisplay: string;
  readonly monthNetDisplay: string;
  readonly billionDisplay: string;
  readonly billionProgress: number;
}

export function SummaryStrip(props: SummaryStripProps): ReactNode {
  return (
    <div className={styles.summaryStrip}>
      <StatCard label="Net Worth" value={props.netWorthDisplay} />
      <StatCard label="$1B Progress" value={props.billionDisplay}>
        <BillionProgressBar progress={props.billionProgress} />
      </StatCard>
      <StatCard label="Cash" value={props.cashDisplay} />
      <StatCard label="Month Net (30d)" value={props.monthNetDisplay} />
    </div>
  );
}
