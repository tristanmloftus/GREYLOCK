// Greylock — top chrome
// =============================================================================
// AGENT-UI (Phase 4). Pure presentational server component. The sync-pulse dot
// color is computed server-side from `lastSyncAt` (≤ 30 min green, 30–60 amber,
// > 60 red). No third-party links. No analytics.
// =============================================================================

import type { ReactNode } from 'react';
import Link from 'next/link';

import styles from './Header.module.css';

export interface HeaderProps {
  readonly brand: string;
  readonly subtitle: string;
  readonly lastSyncAt: Date | null;
  readonly now?: Date;
  /** If true, hides the CONNECT BANK link (e.g. on /login). */
  readonly hideConnect?: boolean;
}

function pulseClass(lastSyncAt: Date | null, now: Date): string {
  if (lastSyncAt === null) {
    return styles.dotRed ?? '';
  }
  const ageMs = now.getTime() - lastSyncAt.getTime();
  const minutes = ageMs / 60_000;
  if (minutes <= 30) {
    return styles.dotGreen ?? '';
  }
  if (minutes <= 60) {
    return styles.dotAmber ?? '';
  }
  return styles.dotRed ?? '';
}

function formatTimestamp(d: Date | null): string {
  if (d === null) {
    return 'never';
  }
  // Locale-independent zero-padded HH:MM display.
  const hh = String(d.getHours()).padStart(2, '0');
  const mm = String(d.getMinutes()).padStart(2, '0');
  const yyyy = d.getFullYear();
  const mo = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  return `${yyyy}-${mo}-${dd} ${hh}:${mm}`;
}

export function Header(props: HeaderProps): ReactNode {
  const now = props.now ?? new Date();
  const dotCls = pulseClass(props.lastSyncAt, now);
  const tsLabel = formatTimestamp(props.lastSyncAt);
  return (
    <header className={styles.header}>
      <div className={styles.brand}>
        <span className={styles.brandMark}>{props.brand}</span>
        <span className={styles.brandSub}>{props.subtitle}</span>
      </div>
      <div className={styles.right}>
        <span className={styles.pulseGroup} aria-label="Last sync">
          <span className={`${styles.dot} ${dotCls}`} aria-hidden="true" />
          <span className="num">SYNC {tsLabel}</span>
        </span>
        {props.hideConnect === true ? null : (
          <Link href="/connect" className={styles.connectButton}>
            CONNECT BANK
          </Link>
        )}
      </div>
    </header>
  );
}
