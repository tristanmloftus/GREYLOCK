// Greylock — admin enroll page (owner-only)
// =============================================================================
// AGENT-UI (Phase 4). Owner gate: 404 for non-owner, redirect to /login if
// no session. Renders <EnrollForm> + <RevokeForm> side-by-side.
// =============================================================================

import type { ReactNode } from 'react';
import { notFound, redirect } from 'next/navigation';

import { Header } from '../../../components/chrome/Header';
import { Footer } from '../../../components/chrome/Footer';
import { EnrollForm } from '../../../components/admin/EnrollForm';
import { RevokeForm } from '../../../components/admin/RevokeForm';

import { resolveCurrentUser } from '../../_lib/current-user';

import styles from '../page.module.css';

export const metadata = {
  title: 'Enroll — Greylock Admin',
};

export const dynamic = 'force-dynamic';

export default async function AdminEnrollPage(): Promise<ReactNode> {
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
        <h1 className={styles.title}>Enroll / Revoke</h1>
        <div className={styles.cardGrid}>
          <EnrollForm />
          <RevokeForm />
        </div>
      </main>
      <Footer />
    </>
  );
}
