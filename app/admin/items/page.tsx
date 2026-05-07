// Greylock — admin items page (owner-only)
// =============================================================================
// AGENT-UI (Phase 4). Owner gate. Renders <ItemList>.
// =============================================================================

import type { ReactNode } from 'react';
import { notFound, redirect } from 'next/navigation';

import { Header } from '../../../components/chrome/Header';
import { Footer } from '../../../components/chrome/Footer';
import { ItemList } from '../../../components/admin/ItemList';

import { resolveCurrentUser } from '../../_lib/current-user';

import styles from '../page.module.css';

export const metadata = {
  title: 'Items — Greylock Admin',
};

export const dynamic = 'force-dynamic';

export default async function AdminItemsPage(): Promise<ReactNode> {
  const ctx = await resolveCurrentUser();
  if (ctx === null) {
    redirect('/login');
  }
  if (ctx.user.role !== 'owner') {
    notFound();
  }
  return (
    <>
      <Header brand="LOFTUS" subtitle="ADMIN — ITEMS" lastSyncAt={null} hideConnect />
      <main className={`app-shell ${styles.wrap}`}>
        <h1 className={styles.title}>Connected items</h1>
        <ItemList />
      </main>
      <Footer />
    </>
  );
}
