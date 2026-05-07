// Greylock — generic error boundary
// =============================================================================
// AGENT-UI (Phase 4). NEVER echoes any underlying error.kind, message, or
// digest from server-side errors. Per QA-SEC Phase-3 §5.1: the user gets
// "Request failed. Please retry." and a way back to the dashboard.
// =============================================================================

'use client';

import type { ReactNode } from 'react';

import styles from './login/page.module.css';

export interface ErrorBoundaryProps {
  readonly error: Error & { readonly digest?: string };
  readonly reset: () => void;
}

export default function ErrorBoundary(props: ErrorBoundaryProps): ReactNode {
  // We deliberately ignore `props.error` — the user has no need for it.
  void props.error;
  return (
    <main className={styles.wrap}>
      <div>
        <div className={styles.subtitle}>Greylock</div>
        <h1 className={styles.title}>Request failed. Please retry.</h1>
      </div>
      <button type="button" onClick={props.reset}>
        Retry
      </button>
    </main>
  );
}
