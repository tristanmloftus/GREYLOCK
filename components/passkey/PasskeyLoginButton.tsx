// Greylock — passkey login (client)
// =============================================================================
// AGENT-UI (Phase 4). Client component. The flow:
//   1. POST /api/auth/authentication/begin with { email }.
//   2. Run startAuthentication(...) on the returned options.
//   3. POST /api/auth/authentication/complete with the assertion.
//   4. Server sets the session cookie and returns 200; we navigate to "/".
//
// Error handling: every API failure surfaces as the generic
// "Request failed. Please retry." message. We never echo `error.kind`.
// =============================================================================

'use client';

import { useCallback, useState, useTransition } from 'react';
import type { ReactNode } from 'react';
import { useRouter } from 'next/navigation';

import { startAuthentication } from '@simplewebauthn/browser';

import styles from './PasskeyLoginButton.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

interface BeginResponse {
  readonly challenge: string;
  readonly rpId: string;
  readonly timeout: number;
  readonly userVerification: 'required';
  readonly allowCredentials: ReadonlyArray<{ readonly id: string; readonly type: 'public-key' }>;
}

export function PasskeyLoginButton(): ReactNode {
  const router = useRouter();
  const [email, setEmail] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [pending, startTransition] = useTransition();

  const onSubmit = useCallback(
    (e: React.FormEvent<HTMLFormElement>): void => {
      e.preventDefault();
      setError(null);
      setStatus('Authenticating…');
      startTransition(() => {
        void (async () => {
          try {
            // Begin.
            const beginRes = await fetch('/api/auth/authentication/begin', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ email }),
              credentials: 'same-origin',
            });
            if (!beginRes.ok) {
              setError(GENERIC_ERROR);
              setStatus(null);
              return;
            }
            const beginBody = (await beginRes.json()) as BeginResponse;

            // WebAuthn assertion. SimpleWebAuthn v11 wraps options in
            // `{ optionsJSON }`; the inner shape is the standard
            // `PublicKeyCredentialRequestOptionsJSON`.
            const assertion = await startAuthentication({
              optionsJSON: {
                challenge: beginBody.challenge,
                rpId: beginBody.rpId,
                timeout: beginBody.timeout,
                userVerification: beginBody.userVerification,
                allowCredentials: beginBody.allowCredentials.map((c) => ({ id: c.id, type: c.type })),
              },
            });

            // Complete.
            const completeRes = await fetch('/api/auth/authentication/complete', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ email, response: assertion }),
              credentials: 'same-origin',
            });
            if (!completeRes.ok) {
              setError(GENERIC_ERROR);
              setStatus(null);
              return;
            }
            setStatus('Redirecting…');
            router.push('/');
            router.refresh();
          } catch {
            setError(GENERIC_ERROR);
            setStatus(null);
          }
        })();
      });
    },
    [email, router],
  );

  return (
    <form className={styles.form} onSubmit={onSubmit} noValidate>
      <div className={styles.row}>
        <label htmlFor="login-email" className={styles.label}>
          Email
        </label>
        <input
          id="login-email"
          type="email"
          autoComplete="username webauthn"
          required
          value={email}
          onChange={(e) => setEmail(e.target.value)}
          className={styles.input}
          placeholder="you@example.com"
          disabled={pending}
        />
      </div>
      <button type="submit" className={styles.submit} disabled={pending || email.length === 0}>
        {pending ? 'Authenticating…' : 'Sign in with passkey'}
      </button>
      {status !== null ? <span className={styles.status}>{status}</span> : null}
      {error !== null ? <span className={styles.error}>{error}</span> : null}
    </form>
  );
}
