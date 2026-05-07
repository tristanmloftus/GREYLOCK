// Greylock — admin audit viewer (client)
// =============================================================================
// AGENT-UI (Phase 4). Owner-only. Calls /api/admin/audit/query with filters
// and renders a table. We never expose token-shape values; the API has
// already sanitized.
// =============================================================================

'use client';

import { useCallback, useState } from 'react';
import type { ReactNode } from 'react';

import styles from './AdminForms.module.css';

const GENERIC_ERROR = 'Request failed. Please retry.';

interface AuditEntryWire {
  readonly seq: string;
  readonly ts: string;
  readonly tsNanos: number;
  readonly actorUserId: string | null;
  readonly actorKind: string;
  readonly domain: string | null;
  readonly subjectId: string | null;
  readonly subjectKind: string | null;
  readonly action: string;
  readonly outcome: string;
  readonly detailsJson: string;
  readonly prevHash: string;
  readonly entryHash: string;
}

interface QueryResponse {
  readonly entries: ReadonlyArray<AuditEntryWire>;
}

export function AuditViewer(): ReactNode {
  const [action, setAction] = useState('');
  const [domain, setDomain] = useState<'' | 'personal' | 'pcc'>('');
  const [actorUserId, setActorUserId] = useState('');
  const [limit, setLimit] = useState('100');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [entries, setEntries] = useState<ReadonlyArray<AuditEntryWire>>([]);

  const onSubmit = useCallback(
    (e: React.FormEvent<HTMLFormElement>): void => {
      e.preventDefault();
      setError(null);
      setPending(true);
      void (async () => {
        try {
          const params = new URLSearchParams();
          if (action.length > 0) {
            params.set('action', action);
          }
          if (domain.length > 0) {
            params.set('domain', domain);
          }
          if (actorUserId.length > 0) {
            params.set('actorUserId', actorUserId);
          }
          if (limit.length > 0) {
            params.set('limit', limit);
          }
          const res = await fetch(`/api/admin/audit/query?${params.toString()}`, {
            credentials: 'same-origin',
          });
          if (!res.ok) {
            setError(GENERIC_ERROR);
            return;
          }
          const body = (await res.json()) as QueryResponse;
          setEntries(body.entries);
        } catch {
          setError(GENERIC_ERROR);
        } finally {
          setPending(false);
        }
      })();
    },
    [action, domain, actorUserId, limit],
  );

  return (
    <section className={styles.card}>
      <span className={styles.title}>Audit query</span>
      <form onSubmit={onSubmit} noValidate>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="audit-action">
            Action
          </label>
          <input
            id="audit-action"
            type="text"
            value={action}
            onChange={(e) => setAction(e.target.value)}
            placeholder="e.g. session_created"
            disabled={pending}
          />
        </div>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="audit-domain">
            Domain
          </label>
          <select
            id="audit-domain"
            value={domain}
            onChange={(e) => {
              const v = e.target.value;
              setDomain(v === 'personal' || v === 'pcc' ? v : '');
            }}
            disabled={pending}
          >
            <option value="">any</option>
            <option value="personal">personal</option>
            <option value="pcc">pcc</option>
          </select>
        </div>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="audit-actor">
            Actor user id
          </label>
          <input
            id="audit-actor"
            type="text"
            value={actorUserId}
            onChange={(e) => setActorUserId(e.target.value)}
            disabled={pending}
          />
        </div>
        <div className={styles.row}>
          <label className={styles.label} htmlFor="audit-limit">
            Limit
          </label>
          <input
            id="audit-limit"
            type="number"
            min={1}
            max={1000}
            value={limit}
            onChange={(e) => setLimit(e.target.value)}
            disabled={pending}
          />
        </div>
        <div className={styles.actions}>
          <button type="submit" className={styles.submit} disabled={pending}>
            {pending ? 'Loading…' : 'Run query'}
          </button>
          {error !== null ? <span className={styles.error}>{error}</span> : null}
        </div>
      </form>
      {entries.length > 0 ? (
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">seq</th>
              <th scope="col">ts</th>
              <th scope="col">actor</th>
              <th scope="col">domain</th>
              <th scope="col">action</th>
              <th scope="col">outcome</th>
            </tr>
          </thead>
          <tbody>
            {entries.map((e) => (
              <tr key={e.seq}>
                <td className="num">{e.seq}</td>
                <td>{e.ts}</td>
                <td>{e.actorUserId ?? `(${e.actorKind})`}</td>
                <td>{e.domain ?? '—'}</td>
                <td>{e.action}</td>
                <td>{e.outcome}</td>
              </tr>
            ))}
          </tbody>
        </table>
      ) : null}
    </section>
  );
}
