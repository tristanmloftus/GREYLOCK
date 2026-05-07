// Greylock — admin enroll form (client)
// =============================================================================
// AGENT-UI (Phase 4). Owner-only. Mints an enrollment URL via
// /api/admin/enroll and displays the cleartext URL once. The cleartext is
// generated server-side; we render it but never log or persist it.
// =============================================================================

'use client';

import { useCallback, useState } from 'react';
import type { ReactNode } from 'react';

import styles from './AdminForms.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

interface EnrollResponse {
  readonly enrollmentUrl: string;
  readonly expiresAt: string;
}

export function EnrollForm(): ReactNode {
  const [email, setEmail] = useState('');
  const [displayName, setDisplayName] = useState('');
  const [role, setRole] = useState<'owner' | 'member'>('member');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [result, setResult] = useState<EnrollResponse | null>(null);

  const onSubmit = useCallback(
    (e: React.FormEvent<HTMLFormElement>): void => {
      e.preventDefault();
      setError(null);
      setResult(null);
      setPending(true);
      void (async () => {
        try {
          const res = await fetch('/api/admin/enroll', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ email, displayName, role }),
            credentials: 'same-origin',
          });
          if (!res.ok) {
            setError(GENERIC_ERROR);
            setPending(false);
            return;
          }
          const body = (await res.json()) as EnrollResponse;
          setResult(body);
        } catch {
          setError(GENERIC_ERROR);
        } finally {
          setPending(false);
        }
      })();
    },
    [email, displayName, role],
  );

  return (
    <section className={styles.card}>
      <span className={styles.title}>Mint enrollment URL</span>
      <form onSubmit={onSubmit} noValidate>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="enroll-email-admin">
            Email
          </label>
          <input
            id="enroll-email-admin"
            type="email"
            required
            value={email}
            onChange={(e) => setEmail(e.target.value)}
            disabled={pending}
          />
        </div>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="enroll-display-admin">
            Display name
          </label>
          <input
            id="enroll-display-admin"
            type="text"
            required
            value={displayName}
            onChange={(e) => setDisplayName(e.target.value)}
            disabled={pending}
          />
        </div>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="enroll-role-admin">
            Role
          </label>
          <select
            id="enroll-role-admin"
            value={role}
            onChange={(e) => setRole(e.target.value === 'owner' ? 'owner' : 'member')}
            disabled={pending}
          >
            <option value="member">member</option>
            <option value="owner">owner</option>
          </select>
        </div>
        <div className={styles.actions}>
          <button type="submit" className={styles.submit} disabled={pending}>
            {pending ? 'Minting…' : 'Mint URL'}
          </button>
          {error !== null ? <span className={styles.error}>{error}</span> : null}
        </div>
      </form>
      {result !== null ? (
        <div className={styles.row}>
          <span className={styles.label}>One-shot enrollment URL (copy now)</span>
          <div className={styles.urlBlock}>{result.enrollmentUrl}</div>
          <span className={styles.status}>Expires {result.expiresAt}</span>
        </div>
      ) : null}
    </section>
  );
}
