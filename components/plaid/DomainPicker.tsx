// Greylock — Plaid domain picker (me / pcc)
// =============================================================================
// AGENT-UI (Phase 4). Client component (controlled by parent state). The PCC
// option is disabled if the current user is not in PccMembership; the parent
// passes `pccAvailable=false` in that case (computed server-side).
// =============================================================================

'use client';

import type { ReactNode } from 'react';

import styles from './DomainPicker.module.css';

export type Domain = 'personal' | 'pcc';

export interface DomainPickerProps {
  readonly value: Domain;
  readonly onChange: (next: Domain) => void;
  readonly pccAvailable: boolean;
  readonly disabled?: boolean;
}

export function DomainPicker(props: DomainPickerProps): ReactNode {
  const onChange = props.onChange;
  return (
    <div className={styles.wrapper}>
      <span className={styles.heading}>Where should this connection live?</span>
      <div className={styles.options}>
        <label
          className={`${styles.option} ${props.value === 'personal' ? styles.optionSelected ?? '' : ''}`}
        >
          <input
            type="radio"
            name="domain"
            value="personal"
            checked={props.value === 'personal'}
            onChange={() => onChange('personal')}
            className={styles.radio}
            disabled={props.disabled === true}
          />
          <span className={styles.body}>
            <span className={styles.title}>Personal (me)</span>
            <span className={styles.description}>
              Encrypted with your passkey-derived key. Only readable while you are signed in.
            </span>
          </span>
        </label>
        <label
          className={`${styles.option} ${props.value === 'pcc' ? styles.optionSelected ?? '' : ''} ${
            !props.pccAvailable ? styles.disabled ?? '' : ''
          }`}
          title={!props.pccAvailable ? 'PCC member required' : undefined}
        >
          <input
            type="radio"
            name="domain"
            value="pcc"
            checked={props.value === 'pcc'}
            onChange={() => onChange('pcc')}
            disabled={props.disabled === true || !props.pccAvailable}
            className={styles.radio}
          />
          <span className={styles.body}>
            <span className={styles.title}>PCC (shared)</span>
            <span className={styles.description}>
              Encrypted with the PCC server key so the 15-min sync runs 24/7. Visible to all PCC
              members.
            </span>
          </span>
        </label>
      </div>
    </div>
  );
}
