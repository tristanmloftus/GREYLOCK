// Greylock — 30s dashboard poller (client)
// =============================================================================
// AGENT-UI (Phase 4). Pauses on `document.visibilityState === 'hidden'` and
// resumes on visibilitychange. On every successful poll, calls router.refresh()
// so the server component re-runs with fresh data. If the API returns 401, we
// surface a "Session expired" modal and link to /login.
// =============================================================================

'use client';

import { useEffect, useState } from 'react';
import type { ReactNode } from 'react';
import { useRouter } from 'next/navigation';
import Link from 'next/link';

import { Modal } from '../modal/Modal';

const POLL_INTERVAL_MS = 30_000;

export interface DashboardPollerProps {
  /** Domain we're displaying. Personal users only poll personal; PCC members
   *  poll both — but the poller only talks to a single endpoint at a time;
   *  the parent server component decides what to render. */
  readonly domain: 'personal' | 'pcc';
}

export function DashboardPoller(props: DashboardPollerProps): ReactNode {
  const router = useRouter();
  const [sessionExpired, setSessionExpired] = useState(false);

  useEffect(() => {
    let timer: ReturnType<typeof setInterval> | null = null;
    let cancelled = false;

    const poll = async (): Promise<void> => {
      if (cancelled || sessionExpired || document.visibilityState === 'hidden') {
        return;
      }
      try {
        const url = `/api/dashboard/snapshot?domain=${props.domain}`;
        const res = await fetch(url, { credentials: 'same-origin' });
        if (res.status === 401) {
          setSessionExpired(true);
          return;
        }
        if (!res.ok) {
          // Soft-fail; we'll try again next interval. No noisy logging.
          return;
        }
        // The server component re-renders on refresh().
        router.refresh();
      } catch {
        // Network blip — try again at next interval.
      }
    };

    const start = (): void => {
      if (timer !== null) {
        return;
      }
      timer = setInterval(() => {
        void poll();
      }, POLL_INTERVAL_MS);
    };
    const stop = (): void => {
      if (timer !== null) {
        clearInterval(timer);
        timer = null;
      }
    };

    const onVisibility = (): void => {
      if (document.visibilityState === 'visible') {
        start();
      } else {
        stop();
      }
    };

    document.addEventListener('visibilitychange', onVisibility);
    if (document.visibilityState === 'visible') {
      start();
    }
    return () => {
      cancelled = true;
      stop();
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [props.domain, router, sessionExpired]);

  return (
    <Modal
      open={sessionExpired}
      title="Session expired"
      onClose={() => setSessionExpired(false)}
      actions={
        <Link href="/login" className="fg-blue">
          Sign in again
        </Link>
      }
    >
      <p>Session expired. Please log in again.</p>
    </Modal>
  );
}
