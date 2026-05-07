// Greylock — admin audit viewer page (owner-only)
// =============================================================================
// AGENT-UI (Phase 4). Owner gate. Renders <AuditViewer>.
// =============================================================================

import type { ReactNode } from 'react';
import { notFound, redirect } from 'next/navigation';

import { Header } from '../../../components/chrome/Header';
import { Footer } from '../../../components/chrome/Footer';
import { AuditViewer } from '../../../components/admin/AuditViewer';

import { resolveCurrentUser } from '../../_lib/current-user';

import styles from '../page.module.css';

export const metadata = {
  title: 'Audit — Greylock Admin',
};

export const dynamic = 'force-dynamic';

export default async function AdminAuditPage(): Promise<ReactNode> {
  const ctx = await resolveCurrentUser();
  if (ctx === null) {
    redirect('/login');
  }
  if (ctx.user.role !== 'owner') {
    notFound();
  }
  return (
    <>
      <Header brand="LOFTUS" subtitle="ADMIN — AUDIT" lastSyncAt={null} hideConnect />
      <main className={`app-shell ${styles.wrap}`}>
        <h1 className={styles.title}>Audit log</h1>
        <AuditViewer />
      </main>
      <Footer />
    </>
  );
}
