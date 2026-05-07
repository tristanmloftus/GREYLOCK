// Greylock — bottom chrome
// =============================================================================
// AGENT-UI (Phase 4). Static, no telemetry, no external links.
// =============================================================================

import type { ReactNode } from 'react';

import styles from './Footer.module.css';

export function Footer(): ReactNode {
  return (
    <footer className={styles.footer}>
      <span>NOT FINANCIAL ADVICE · localhost-only · v0.1.0-alpha</span>
    </footer>
  );
}
