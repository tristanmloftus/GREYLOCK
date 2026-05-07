// Greylock — Plaid Link launcher
// =============================================================================
// AGENT-UI (Phase 4). Client component. Loads Plaid Link from cdn.plaid.com via
// `next/script` (the single accepted third-party origin per the brief, to be
// allowlisted in the Phase-5 CSP). Flow:
//
//   1. Render the domain picker.
//   2. POST /api/plaid/link-token { domain, products: ['transactions'] } → linkToken.
//   3. Plaid.create({ token: linkToken, onSuccess, onExit }) → handler.open().
//   4. On onSuccess: POST /api/plaid/exchange { domain, publicToken, ... } → server
//      encrypts and persists. We then router.push('/').
//
// The Plaid Link script registers a global `Plaid` object. We type that
// minimally rather than pulling in `react-plaid-link` so we keep the bundle
// surface small. (See retro for size justification.)
// =============================================================================

'use client';

import { useCallback, useState } from 'react';
import type { ReactNode } from 'react';
import { useRouter } from 'next/navigation';
import Script from 'next/script';

import { DomainPicker } from './DomainPicker';
import type { Domain } from './DomainPicker';

import styles from './PlaidLinkButton.module.css';

const GENERIC_ERROR = 'Bank connection failed. Please retry.';
const PLAID_LINK_SRC = 'https://cdn.plaid.com/link/v2/stable/link-initialize.js';

interface PlaidLinkHandler {
  readonly open: () => void;
  readonly destroy: () => void;
}

interface PlaidLinkConfig {
  readonly token: string;
  readonly onSuccess: (publicToken: string, metadata: PlaidLinkSuccessMetadata) => void;
  readonly onExit?: (error: unknown, metadata: unknown) => void;
}

interface PlaidLinkSuccessMetadata {
  readonly institution?: { readonly institution_id: string | null; readonly name: string | null };
}

interface PlaidGlobal {
  readonly create: (config: PlaidLinkConfig) => PlaidLinkHandler;
}

declare global {
  interface Window {
    Plaid?: PlaidGlobal;
  }
}

export interface PlaidLinkButtonProps {
  readonly pccAvailable: boolean;
  /**
   * Test-only injection. When set, the component skips the real Plaid script
   * and uses this stub to simulate a successful Link return. Production code
   * never sets this. Wired by the e2e tests via `(window as any).__greylockPlaidStub`.
   */
  readonly _testStub?: PlaidGlobal;
}

export function PlaidLinkButton(props: PlaidLinkButtonProps): ReactNode {
  const router = useRouter();
  const [domain, setDomain] = useState<Domain>('personal');
  const [scriptLoaded, setScriptLoaded] = useState(false);
  const [pending, setPending] = useState(false);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const handleClick = useCallback((): void => {
    setError(null);
    setStatus('Requesting link token…');
    setPending(true);

    void (async () => {
      try {
        const tokenRes = await fetch('/api/plaid/link-token', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ domain, products: ['transactions'] }),
          credentials: 'same-origin',
        });
        if (!tokenRes.ok) {
          setError(GENERIC_ERROR);
          setStatus(null);
          setPending(false);
          return;
        }
        const tokenBody = (await tokenRes.json()) as { readonly linkToken: string };

        const stub = props._testStub ?? extractPlaidGlobalForTests();
        const plaid = stub ?? window.Plaid;
        if (plaid === undefined) {
          setError(GENERIC_ERROR);
          setStatus(null);
          setPending(false);
          return;
        }

        const handler = plaid.create({
          token: tokenBody.linkToken,
          onSuccess: (publicToken, metadata) => {
            void (async () => {
              setStatus('Encrypting and saving…');
              try {
                const exchangeRes = await fetch('/api/plaid/exchange', {
                  method: 'POST',
                  headers: { 'Content-Type': 'application/json' },
                  body: JSON.stringify({
                    domain,
                    publicToken,
                    institutionId: metadata.institution?.institution_id ?? null,
                    institutionName: metadata.institution?.name ?? null,
                  }),
                  credentials: 'same-origin',
                });
                if (!exchangeRes.ok) {
                  setError(GENERIC_ERROR);
                  setStatus(null);
                  setPending(false);
                  return;
                }
                setStatus('Connected. Redirecting…');
                router.push('/');
                router.refresh();
              } catch {
                setError(GENERIC_ERROR);
                setStatus(null);
                setPending(false);
              }
            })();
          },
          onExit: () => {
            setPending(false);
            setStatus(null);
          },
        });
        setStatus('Opening Plaid…');
        handler.open();
      } catch {
        setError(GENERIC_ERROR);
        setStatus(null);
        setPending(false);
      }
    })();
  }, [domain, props._testStub, router]);

  return (
    <div className={styles.wrapper}>
      {/* Plaid script loaded with strategy="afterInteractive" — explicit user
          gesture required before we use it. CSP exception accepted in the brief. */}
      <Script
        src={PLAID_LINK_SRC}
        strategy="afterInteractive"
        onLoad={() => setScriptLoaded(true)}
      />
      <DomainPicker
        value={domain}
        onChange={setDomain}
        pccAvailable={props.pccAvailable}
        disabled={pending}
      />
      <div className={styles.actionRow}>
        <button
          type="button"
          className={styles.connectButton}
          onClick={handleClick}
          disabled={pending || (props._testStub === undefined && !scriptLoaded)}
        >
          {pending ? 'Connecting…' : 'Connect bank'}
        </button>
        {status !== null ? <span className={styles.status}>{status}</span> : null}
      </div>
      {error !== null ? <span className={styles.error}>{error}</span> : null}
    </div>
  );
}

/**
 * Hook for Playwright tests: tests can inject a stub onto
 * `window.__greylockPlaidStub` before clicking. We pick it up here so we can
 * drive Plaid Link without loading the real script.
 */
function extractPlaidGlobalForTests(): PlaidGlobal | undefined {
  if (typeof window === 'undefined') {
    return undefined;
  }
  const w = window as unknown as { __greylockPlaidStub?: PlaidGlobal };
  return w.__greylockPlaidStub;
}
