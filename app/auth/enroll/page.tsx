// Greylock — passkey enrollment landing page
// =============================================================================
// AGENT-UI (Phase 4). Reachable without an existing session. Reads
// `?email=<...>&token=<...>` from the URL and forwards them to the client
// component. The token is the one-shot URL token minted by `pnpm admin:enroll`
// (or POST /api/admin/enroll). Server component shell only; the actual fetch
// + WebAuthn ceremony runs in <PasskeyEnrollButton>.
// =============================================================================

import type { ReactNode } from 'react';

import { Header } from '../../../components/chrome/Header';
import { Footer } from '../../../components/chrome/Footer';
import { PasskeyEnrollButton } from '../../../components/passkey/PasskeyEnrollButton';

import styles from '../../login/page.module.css';

export const metadata = {
  title: 'Enroll — Greylock',
};

interface EnrollPageProps {
  readonly searchParams?: Promise<{
    readonly email?: string | string[];
    readonly token?: string | string[];
  }>;
}

function asString(v: string | string[] | undefined): string {
  if (Array.isArray(v)) {
    return v[0] ?? '';
  }
  return v ?? '';
}

export default async function EnrollPage(props: EnrollPageProps): Promise<ReactNode> {
  const sp = (await props.searchParams) ?? {};
  const email = asString(sp.email).trim().toLowerCase();
  const token = asString(sp.token).trim();
  const valid = email.length > 0 && token.length > 0;

  return (
    <>
      <Header brand="LOFTUS" subtitle="OPERATING DASHBOARD" lastSyncAt={null} hideConnect />
      <main className={styles.wrap}>
        <div>
          <div className={styles.subtitle}>Greylock</div>
          <h1 className={styles.title}>Enroll passkey</h1>
        </div>
        {valid ? (
          <PasskeyEnrollButton email={email} token={token} />
        ) : (
          <p className={styles.notice}>
            Invalid enrollment link. Use the URL printed by <code>pnpm admin:enroll</code>.
          </p>
        )}
      </main>
      <Footer />
    </>
  );
}
