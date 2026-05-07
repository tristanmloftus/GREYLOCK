// Greylock — accessible modal dialog
// =============================================================================
// AGENT-UI (Phase 4). Client component. Implements:
//   - focus trap (basic): focuses dialog on open, restores focus on close
//   - Esc-to-close
//   - click-outside-to-close
// CSP-friendly: no inline event handlers in markup; React adds listeners via
// addEventListener.
// =============================================================================

'use client';

import {
  useCallback,
  useEffect,
  useRef,
  type ReactNode,
} from 'react';

import styles from './Modal.module.css';

export interface ModalProps {
  readonly open: boolean;
  readonly title: string;
  readonly onClose: () => void;
  readonly children?: ReactNode;
  readonly actions?: ReactNode;
}

export function Modal(props: ModalProps): ReactNode {
  const dialogRef = useRef<HTMLDivElement | null>(null);
  const previouslyFocused = useRef<HTMLElement | null>(null);
  const onClose = props.onClose;

  const handleBackdropClick = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      if (e.target === e.currentTarget) {
        onClose();
      }
    },
    [onClose],
  );

  useEffect(() => {
    if (!props.open) {
      return;
    }
    previouslyFocused.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    dialogRef.current?.focus();
    const handleKey = (e: KeyboardEvent): void => {
      if (e.key === 'Escape') {
        onClose();
      }
    };
    window.addEventListener('keydown', handleKey);
    return () => {
      window.removeEventListener('keydown', handleKey);
      previouslyFocused.current?.focus();
    };
  }, [props.open, onClose]);

  if (!props.open) {
    return null;
  }

  return (
    <div
      className={styles.backdrop}
      role="presentation"
      onClick={handleBackdropClick}
    >
      <div
        ref={dialogRef}
        className={styles.dialog}
        role="dialog"
        aria-modal="true"
        aria-label={props.title}
        tabIndex={-1}
      >
        <div className={styles.title}>{props.title}</div>
        {props.children !== undefined ? <div className={styles.body}>{props.children}</div> : null}
        {props.actions !== undefined ? <div className={styles.actions}>{props.actions}</div> : null}
      </div>
    </div>
  );
}
