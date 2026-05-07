// Greylock — Plaid Link bootstrap page
// =============================================================================
// AGENT-UI (Phase 4). Authenticated. The server component pre-resolves
// PccMembership and passes it to the client `<PlaidLinkButton>` so the
// domain picker can disable PCC for non-members at first paint.
// =============================================================================

import type { ReactNode } from 'react';
import { redirect } from 'next/navigation';

import { Header } from '../../components/chrome/Header';
import { Footer } from '../../components/chrome/Footer';
import { PlaidLinkButton } from '../../components/plaid/PlaidLinkButton';

import { resolveCurrentUser } from '../_lib/current-user';

import styles from './page.module.css';

export const metadata = {
  title: 'Connect — Greylock',
};

export const dynamic = 'force-dynamic';

export default async function ConnectPage(): Promise<ReactNode> {
  const ctx = await resolveCurrentUser();
  if (ctx === null) {
    redirect('/login');
  }
  return (
    <>
      <Header brand="LOFTUS" subtitle="CONNECT BANK" lastSyncAt={null} hideConnect />
      <main className={`app-shell ${styles.wrap}`}>
        <h1 className={styles.title}>Connect a bank</h1>
        <p className={styles.intro}>
          Pick a domain and tap Connect bank. Plaid Link runs entirely in your browser; the
          access_token never leaves the server, and we encrypt it before persistence.
        </p>
        <PlaidLinkButton pccAvailable={ctx.isPccMember} />
      </main>
      <Footer />
    </>
  );
}
