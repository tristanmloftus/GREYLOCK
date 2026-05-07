// Greylock — connected items list with remove action (client)
// =============================================================================
// AGENT-UI (Phase 4). Lists items via /api/plaid/items and lets owners /
// users invoke /api/plaid/items/remove.
// =============================================================================

'use client';

import { useCallback, useEffect, useState } from 'react';
import type { ReactNode } from 'react';

import styles from './AdminForms.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

interface ItemEntry {
  readonly itemId: string;
  readonly domain: 'personal' | 'pcc';
  readonly institutionName: string | null;
  readonly lastSyncAt: string | null;
  readonly lastSyncOutcome: 'success' | 'error' | 'pending' | null;
  readonly consecutiveFailures: number;
  readonly removedAt: string | null;
}

interface ListResponse {
  readonly items: ReadonlyArray<ItemEntry>;
}

export function ItemList(): ReactNode {
  const [items, setItems] = useState<ReadonlyArray<ItemEntry>>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [pendingId, setPendingId] = useState<string | null>(null);

  const reload = useCallback((): void => {
    setLoading(true);
    setError(null);
    void (async () => {
      try {
        const res = await fetch('/api/plaid/items', { credentials: 'same-origin' });
        if (!res.ok) {
          setError(GENERIC_ERROR);
          return;
        }
        const body = (await res.json()) as ListResponse;
        setItems(body.items);
      } catch {
        setError(GENERIC_ERROR);
      } finally {
        setLoading(false);
      }
    })();
  }, []);

  useEffect(() => {
    reload();
  }, [reload]);

  const onRemove = useCallback(
    (itemId: string): void => {
      setPendingId(itemId);
      void (async () => {
        try {
          const res = await fetch('/api/plaid/items/remove', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ itemId, reason: 'admin_remove' }),
            credentials: 'same-origin',
          });
          if (!res.ok) {
            setError(GENERIC_ERROR);
            return;
          }
          reload();
        } catch {
          setError(GENERIC_ERROR);
        } finally {
          setPendingId(null);
        }
      })();
    },
    [reload],
  );

  return (
    <section className={styles.card}>
      <span className={styles.title}>Connected items</span>
      {loading ? <span className={styles.status}>Loading…</span> : null}
      {error !== null ? <span className={styles.error}>{error}</span> : null}
      {!loading && items.length === 0 ? <span className={styles.status}>No items connected.</span> : null}
      {items.length > 0 ? (
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">Institution</th>
              <th scope="col">Domain</th>
              <th scope="col">Last sync</th>
              <th scope="col">Outcome</th>
              <th scope="col">Failures</th>
              <th scope="col" />
            </tr>
          </thead>
          <tbody>
            {items.map((it) => (
              <tr key={it.itemId}>
                <td>{it.institutionName ?? it.itemId}</td>
                <td>{it.domain}</td>
                <td>{it.lastSyncAt ?? '—'}</td>
                <td>{it.lastSyncOutcome ?? '—'}</td>
                <td className="num">{it.consecutiveFailures}</td>
                <td>
                  <button
                    type="button"
                    className={styles.danger}
                    onClick={() => onRemove(it.itemId)}
                    disabled={pendingId === it.itemId || it.removedAt !== null}
                  >
                    {it.removedAt !== null ? 'Removed' : pendingId === it.itemId ? 'Removing…' : 'Remove'}
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      ) : null}
    </section>
  );
}
