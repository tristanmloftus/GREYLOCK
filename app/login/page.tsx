// Greylock — passkey login page (server component shell + client button)
// =============================================================================
// AGENT-UI (Phase 4). Reachable without auth.
// =============================================================================

import type { ReactNode } from 'react';

import { Header } from '../../components/chrome/Header';
import { Footer } from '../../components/chrome/Footer';
import { PasskeyLoginButton } from '../../components/passkey/PasskeyLoginButton';

import styles from './page.module.css';

export const metadata = {
  title: 'Sign in — Greylock',
};

export default function LoginPage(): ReactNode {
  return (
    <>
      <Header
        brand="LOFTUS"
        subtitle="OPERATING DASHBOARD"
        lastSyncAt={null}
        hideConnect
      />
      <main className={styles.wrap}>
        <div>
          <div className={styles.subtitle}>Greylock</div>
          <h1 className={styles.title}>Sign in</h1>
        </div>
        <PasskeyLoginButton />
        <p className={styles.notice}>
          Passkey-only access. No password fallback. If you lose your passkey, the owner must
          re-enroll you from the admin CLI.
        </p>
      </main>
      <Footer />
    </>
  );
}
