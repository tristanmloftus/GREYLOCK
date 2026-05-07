// Greylock — admin landing page (owner-only)
// =============================================================================
// AGENT-UI (Phase 4). Server component. Resolves the current user, gates on
// `role === 'owner'`. Non-owner returns the same 404 as a missing route — no
// role enumeration (per threat model).
// =============================================================================

import type { ReactNode } from 'react';
import { notFound, redirect } from 'next/navigation';
import Link from 'next/link';

import { Header } from '../../components/chrome/Header';
import { Footer } from '../../components/chrome/Footer';

import { resolveCurrentUser } from '../_lib/current-user';

import styles from './page.module.css';

export const metadata = {
  title: 'Admin — Greylock',
};

export const dynamic = 'force-dynamic';

export default async function AdminLandingPage(): Promise<ReactNode> {
  const ctx = await resolveCurrentUser();
  if (ctx === null) {
    redirect('/login');
  }
  if (ctx.user.role !== 'owner') {
    notFound();
  }
  return (
    <>
      <Header brand="LOFTUS" subtitle="ADMIN" lastSyncAt={null} hideConnect />
      <main className={`app-shell ${styles.wrap}`}>
        <h1 className={styles.title}>Admin</h1>
        <div className={styles.cardGrid}>
          <Link className={styles.card} href="/admin/enroll">
            <span className={styles.cardTitle}>Enroll</span>
            <span className={styles.cardDesc}>Mint a one-shot enrollment URL.</span>
          </Link>
          <Link className={styles.card} href="/admin/audit">
            <span className={styles.cardTitle}>Audit</span>
            <span className={styles.cardDesc}>Browse the hash-chained audit log.</span>
          </Link>
          <Link className={styles.card} href="/admin/items">
            <span className={styles.cardTitle}>Items</span>
            <span className={styles.cardDesc}>Connected Plaid items + remove.</span>
          </Link>
          <Link className={styles.card} href="/admin/enroll#revoke">
            <span className={styles.cardTitle}>Revoke</span>
            <span className={styles.cardDesc}>Revoke active sessions for an email.</span>
          </Link>
        </div>
      </main>
      <Footer />
    </>
  );
}
