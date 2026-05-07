// Greylock — passkey enrollment (client)
// =============================================================================
// AGENT-UI (Phase 4). Reads `email` and `token` from the page props (passed in
// by the server component, which read them from `?email=...&token=...`). The
// flow:
//   1. POST /api/auth/registration/begin with { email, displayName } and the
//      `x-enrollment-token` header. Returns WebAuthn registration options.
//   2. Run startRegistration(...) to capture the attestation locally.
//   3. POST /api/auth/registration/complete with the attestation.
//   4. Redirect to /login.
// =============================================================================

'use client';

import { useCallback, useState, useTransition } from 'react';
import type { ReactNode } from 'react';
import { useRouter } from 'next/navigation';

import { startRegistration } from '@simplewebauthn/browser';

import styles from './PasskeyLoginButton.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

export interface PasskeyEnrollButtonProps {
  readonly email: string;
  readonly token: string;
}

interface BeginResponse {
  readonly challenge: string;
  readonly rp: { readonly id: string; readonly name: string };
  readonly user: { readonly id: string; readonly name: string; readonly displayName: string };
  readonly pubKeyCredParams: ReadonlyArray<{ readonly type: 'public-key'; readonly alg: number }>;
  readonly timeout: number;
  readonly attestation: 'none';
  readonly authenticatorSelection: {
    readonly residentKey: 'required';
    readonly userVerification: 'required';
  };
}

export function PasskeyEnrollButton(props: PasskeyEnrollButtonProps): ReactNode {
  const router = useRouter();
  const [displayName, setDisplayName] = useState(props.email.split('@')[0] ?? '');
  const [deviceLabel, setDeviceLabel] = useState('My device');
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [pending, startTransition] = useTransition();

  const onSubmit = useCallback(
    (e: React.FormEvent<HTMLFormElement>): void => {
      e.preventDefault();
      setError(null);
      setStatus('Beginning enrollment…');
      startTransition(() => {
        void (async () => {
          try {
            const beginRes = await fetch('/api/auth/registration/begin', {
              method: 'POST',
              headers: {
                'Content-Type': 'application/json',
                'x-enrollment-token': props.token,
              },
              body: JSON.stringify({ email: props.email, displayName }),
              credentials: 'same-origin',
            });
            if (!beginRes.ok) {
              setError(GENERIC_ERROR);
              setStatus(null);
              return;
            }
            const beginBody = (await beginRes.json()) as BeginResponse;

            setStatus('Tap your authenticator…');
            const attestation = await startRegistration({
              optionsJSON: {
                challenge: beginBody.challenge,
                rp: beginBody.rp,
                user: beginBody.user,
                pubKeyCredParams: beginBody.pubKeyCredParams.map((p) => ({ type: p.type, alg: p.alg })),
                timeout: beginBody.timeout,
                attestation: beginBody.attestation,
                authenticatorSelection: beginBody.authenticatorSelection,
              },
            });

            setStatus('Finalizing…');
            const completeRes = await fetch('/api/auth/registration/complete', {
              method: 'POST',
              headers: {
                'Content-Type': 'application/json',
                'x-enrollment-token': props.token,
              },
              body: JSON.stringify({
                email: props.email,
                response: attestation,
                deviceLabel: deviceLabel.length === 0 ? null : deviceLabel,
              }),
              credentials: 'same-origin',
            });
            if (!completeRes.ok) {
              setError(GENERIC_ERROR);
              setStatus(null);
              return;
            }
            setStatus('Enrolled. Redirecting…');
            router.push('/login');
          } catch {
            setError(GENERIC_ERROR);
            setStatus(null);
          }
        })();
      });
    },
    [displayName, deviceLabel, props.email, props.token, router],
  );

  return (
    <form className={styles.form} onSubmit={onSubmit} noValidate>
      <div className={styles.row}>
        <label htmlFor="enroll-email" className={styles.label}>
          Email
        </label>
        <input id="enroll-email" type="email" value={props.email} readOnly className={styles.input} />
      </div>
      <div className={styles.row}>
        <label htmlFor="enroll-name" className={styles.label}>
          Display name
        </label>
        <input
          id="enroll-name"
          type="text"
          required
          value={displayName}
          onChange={(e) => setDisplayName(e.target.value)}
          className={styles.input}
          disabled={pending}
        />
      </div>
      <div className={styles.row}>
        <label htmlFor="enroll-device" className={styles.label}>
          Device label (optional)
        </label>
        <input
          id="enroll-device"
          type="text"
          value={deviceLabel}
          onChange={(e) => setDeviceLabel(e.target.value)}
          className={styles.input}
          disabled={pending}
        />
      </div>
      <button type="submit" className={styles.submit} disabled={pending}>
        {pending ? 'Enrolling…' : 'Enroll passkey'}
      </button>
      {status !== null ? <span className={styles.status}>{status}</span> : null}
      {error !== null ? <span className={styles.error}>{error}</span> : null}
    </form>
  );
}
