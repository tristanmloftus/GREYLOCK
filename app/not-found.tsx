// Greylock — 404 page
// =============================================================================
// AGENT-UI (Phase 4). Generic 404 in the OVRWCH aesthetic. Used both for
// missing routes and for owner-gated routes hit by non-owners (so a non-owner
// cannot enumerate which admin pages exist).
// =============================================================================

import type { ReactNode } from 'react';
import Link from 'next/link';

import { Header } from '../components/chrome/Header';
import { Footer } from '../components/chrome/Footer';

import styles from './login/page.module.css';

export default function NotFound(): ReactNode {
  return (
    <>
      <Header brand="LOFTUS" subtitle="404" lastSyncAt={null} hideConnect />
      <main className={styles.wrap}>
        <div>
          <div className={styles.subtitle}>Greylock</div>
          <h1 className={styles.title}>Not found</h1>
        </div>
        <p className={styles.notice}>
          The route you requested does not exist on this localhost build.
        </p>
        <Link href="/">Return to dashboard</Link>
      </main>
      <Footer />
    </>
  );
}
