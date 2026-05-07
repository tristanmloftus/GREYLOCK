// Greylock — admin revoke form (client)
// =============================================================================
// AGENT-UI (Phase 4). Owner-only. Revokes all sessions for an email.
// =============================================================================

'use client';

import { useCallback, useState } from 'react';
import type { ReactNode } from 'react';

import styles from './AdminForms.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

interface RevokeResponse {
  readonly sessionsRevoked: number;
}

export function RevokeForm(): ReactNode {
  const [email, setEmail] = useState('');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);

  const onSubmit = useCallback(
    (e: React.FormEvent<HTMLFormElement>): void => {
      e.preventDefault();
      setError(null);
      setStatus(null);
      setPending(true);
      void (async () => {
        try {
          const res = await fetch('/api/admin/revoke', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ email }),
            credentials: 'same-origin',
          });
          if (!res.ok) {
            setError(GENERIC_ERROR);
            setPending(false);
            return;
          }
          const body = (await res.json()) as RevokeResponse;
          setStatus(`Revoked ${body.sessionsRevoked} session(s).`);
        } catch {
          setError(GENERIC_ERROR);
        } finally {
          setPending(false);
        }
      })();
    },
    [email],
  );

  return (
    <section className={styles.card}>
      <span className={styles.title}>Revoke sessions</span>
      <form onSubmit={onSubmit} noValidate>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="revoke-email-admin">
            Email
          </label>
          <input
            id="revoke-email-admin"
            type="email"
            required
            value={email}
            onChange={(e) => setEmail(e.target.value)}
            disabled={pending}
          />
        </div>
        <div className={styles.actions}>
          <button type="submit" className={styles.danger} disabled={pending}>
            {pending ? 'Revoking…' : 'Revoke'}
          </button>
          {status !== null ? <span className={styles.status}>{status}</span> : null}
          {error !== null ? <span className={styles.error}>{error}</span> : null}
        </div>
      </form>
    </section>
  );
}
