// Greylock — IPC keybridge server (web-process side)
// =============================================================================
// AGENT-SYNC (Phase 3). Listens on a Unix-domain socket and serves DEK
// requests from the sync worker. Three layers of defense:
//   1. Filesystem mode 0600 on the socket — only our UID can connect.
//   2. Peer-credential check via `lib/ipc/peer-cred.ts` — reject any
//      connection whose UID differs from ours.
//   3. HMAC-SHA-256 handshake using a key derived from the Master KEK at
//      boot. Without the boot environment, no peer can answer the challenge.
//
// `requestDek` semantics:
//   - { kind: 'pcc' } → serve the loaded PCC DEK.
//   - { userId, sessionId } → look up an active session; if missing/revoked
//     → `session_invalid`; otherwise return the user's DEK.
//
// On `releaseDek` we don't need to do anything server-side (the worker
// owns its borrowed copy). We still return ok so the worker has a clean
// audit trail.
// =============================================================================

import {
  createHmac,
  randomBytes,
  randomUUID,
  timingSafeEqual,
} from 'node:crypto';
import * as fs from 'node:fs';
import { createServer, type Server, type Socket } from 'node:net';

import { Err, Ok } from '../types/domain.js';
import { UserId } from '../types/domain.js';
import type {
  AuditAction,
  Result,
  SessionId,
  UserId as UserIdType,
} from '../types/domain.js';
import type {
  AuditService,
  KeybridgeError,
  KeybridgeServer,
  SessionRepository,
} from '../types/services.js';
import { ActorKind, AuditAction as AuditActionConst, AuditOutcome } from '../types/domain.js';

import {
  AuthSchema,
  HelloSchema,
  PingParamsSchema,
  RequestDekParamsSchema,
  RequestSchema,
  ReleaseDekParamsSchema,
  createLineSplitter,
  encodeLine,
  decodeJson,
} from './keybridge-protocol.js';
import { peerUidMatchesOurs } from './peer-cred.js';

// -----------------------------------------------------------------------------
// Server interface — exposes the in-memory DEK store via a callback so we
// don't import lib/crypto/* directly.
// -----------------------------------------------------------------------------

export interface DekProvider {
  /** Returns the active PCC DEK and its version, or null if not loaded. */
  getPccDek(): { readonly version: number; readonly dek: Uint8Array } | null;
  /** Returns the per-user DEK for the given userId if loaded, or null. */
  getUserDek(userId: UserIdType): { readonly version: number; readonly dek: Uint8Array } | null;
}

/**
 * Construct a DekProvider that reads from a CryptoService instance. Because
 * CryptoService doesn't currently expose raw byte access, this provider must
 * be constructed AT BOOT TIME with the same in-memory references the rest of
 * the web process uses. We wrap that with a tiny adapter shim provided by
 * the boot routine. For tests, a fake DekProvider is injected directly.
 */
export interface KeybridgeServerOptions {
  readonly socketPath: string;
  /** HMAC key — derived at boot from `HKDF(MasterKEK, info='greylock/keybridge/v1')`. */
  readonly hmacKey: Uint8Array;
  readonly dekProvider: DekProvider;
  readonly sessionRepo: SessionRepository;
  readonly audit: AuditService;
  /** Optional override for tests. */
  readonly nowMs?: () => number;
  /** Optional override for tests; default is 5s. */
  readonly handshakeTimeoutMs?: number;
}

const DEFAULT_HANDSHAKE_TIMEOUT_MS = 5_000;

interface ConnectionState {
  socket: Socket;
  splitter: ReturnType<typeof createLineSplitter>;
  phase: 'awaiting_hello' | 'awaiting_auth' | 'authenticated' | 'closed';
  serverNonce: Buffer | null;
  /** Sessions invalidated since this connection authenticated. */
  invalidatedSessions: Set<string>;
}

export function createKeybridgeServer(opts: KeybridgeServerOptions): KeybridgeServer & {
  readonly _testing?: { readonly serverInstance: () => Server | null };
} {
  let server: Server | null = null;
  const connections: Set<ConnectionState> = new Set();

  async function audit(args: {
    readonly action: AuditAction;
    readonly outcome: 'success' | 'failure' | 'denied';
    readonly details: Readonly<Record<string, unknown>>;
  }): Promise<void> {
    try {
      await opts.audit.append({
        actorUserId: null,
        actorKind: ActorKind.System,
        domain: null,
        subjectId: null,
        subjectKind: null,
        action: args.action,
        outcome:
          args.outcome === 'success'
            ? AuditOutcome.Success
            : args.outcome === 'failure'
              ? AuditOutcome.Failure
              : AuditOutcome.Denied,
        details: args.details,
      });
    } catch {
      // Audit MUST NOT block the keybridge. Swallow.
    }
  }

  function dropConnection(state: ConnectionState, reason: string): void {
    void reason;
    if (state.phase === 'closed') {
      return;
    }
    state.phase = 'closed';
    try {
      state.socket.destroy();
    } catch {
      // already destroyed
    }
    connections.delete(state);
  }

  function sendLine(state: ConnectionState, message: object): boolean {
    try {
      state.socket.write(encodeLine(message));
      return true;
    } catch {
      dropConnection(state, 'write_failed');
      return false;
    }
  }

  function sendErrorResponse(
    state: ConnectionState,
    id: string,
    kind:
      | 'session_invalid'
      | 'dek_unavailable'
      | 'protocol_error'
      | 'auth_failed'
      | 'peer_credential_mismatch',
  ): void {
    sendLine(state, {
      type: 'RESPONSE',
      id,
      ok: false,
      error: { kind },
    });
  }

  async function handleRequest(
    state: ConnectionState,
    raw: unknown,
  ): Promise<void> {
    const parsed = RequestSchema.safeParse(raw);
    if (!parsed.success) {
      sendErrorResponse(state, 'unknown', 'protocol_error');
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'failure',
        details: { kind: 'malformed_request' },
      });
      dropConnection(state, 'protocol_error');
      return;
    }
    const req = parsed.data;
    if (req.method === 'ping') {
      const params = PingParamsSchema.safeParse(req.params ?? {});
      if (!params.success) {
        sendErrorResponse(state, req.id, 'protocol_error');
        return;
      }
      sendLine(state, { type: 'RESPONSE', id: req.id, ok: true, result: {} });
      return;
    }
    if (req.method === 'releaseDek') {
      const params = ReleaseDekParamsSchema.safeParse(req.params);
      if (!params.success) {
        sendErrorResponse(state, req.id, 'protocol_error');
        return;
      }
      sendLine(state, { type: 'RESPONSE', id: req.id, ok: true, result: {} });
      return;
    }
    if (req.method !== 'requestDek') {
      sendErrorResponse(state, req.id, 'protocol_error');
      return;
    }

    const params = RequestDekParamsSchema.safeParse(req.params);
    if (!params.success) {
      sendErrorResponse(state, req.id, 'protocol_error');
      return;
    }
    const p = params.data;

    if ('kind' in p && p.kind === 'pcc') {
      const pcc = opts.dekProvider.getPccDek();
      if (pcc === null) {
        sendErrorResponse(state, req.id, 'dek_unavailable');
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'failure',
          details: { kind: 'pcc_dek_not_loaded' },
        });
        return;
      }
      sendLine(state, {
        type: 'RESPONSE',
        id: req.id,
        ok: true,
        result: {
          handle: { kind: 'pcc', version: pcc.version },
          dekB64: Buffer.from(pcc.dek).toString('base64'),
        },
      });
      return;
    }

    // user request — validate active session.
    const userIdRaw = (p as { userId: string }).userId;
    const sessionIdRaw = (p as { sessionId: string }).sessionId;
    const userId = UserId(userIdRaw);
    if (state.invalidatedSessions.has(sessionIdRaw)) {
      sendErrorResponse(state, req.id, 'session_invalid');
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'denied',
        details: { kind: 'session_invalidated' },
      });
      return;
    }
    const sess = await opts.sessionRepo.findActiveByUser(userId);
    if (!sess.ok || sess.value === null) {
      sendErrorResponse(state, req.id, 'session_invalid');
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'denied',
        details: { kind: 'no_active_session' },
      });
      return;
    }
    if ((sess.value.id as unknown as string) !== sessionIdRaw) {
      sendErrorResponse(state, req.id, 'session_invalid');
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'denied',
        details: { kind: 'session_mismatch' },
      });
      return;
    }
    const dek = opts.dekProvider.getUserDek(userId);
    if (dek === null) {
      sendErrorResponse(state, req.id, 'dek_unavailable');
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'failure',
        details: { kind: 'user_dek_not_loaded' },
      });
      return;
    }
    sendLine(state, {
      type: 'RESPONSE',
      id: req.id,
      ok: true,
      result: {
        handle: { kind: 'user', userId: userIdRaw, version: dek.version },
        dekB64: Buffer.from(dek.dek).toString('base64'),
      },
    });
  }

  async function handleLine(state: ConnectionState, line: Buffer): Promise<void> {
    const json = decodeJson(line);
    if (!json.ok) {
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'failure',
        details: { kind: json.kind === 'oversized' ? 'oversized_line' : 'malformed_json' },
      });
      dropConnection(state, 'protocol_error');
      return;
    }

    if (state.phase === 'awaiting_hello') {
      const hello = HelloSchema.safeParse(json.value);
      if (!hello.success) {
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'failure',
          details: { kind: 'hello_invalid' },
        });
        dropConnection(state, 'protocol_error');
        return;
      }
      const serverNonce = randomBytes(32);
      state.serverNonce = serverNonce;
      state.phase = 'awaiting_auth';
      const sent = sendLine(state, {
        type: 'HELLO_OK',
        v: 1,
        serverNonce: serverNonce.toString('base64'),
      });
      if (!sent) {
        return;
      }
      return;
    }

    if (state.phase === 'awaiting_auth') {
      const auth = AuthSchema.safeParse(json.value);
      if (!auth.success || state.serverNonce === null) {
        sendLine(state, { type: 'AUTH_DENY', reason: 'auth_failed' });
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'denied',
          details: { kind: 'auth_failed' },
        });
        dropConnection(state, 'auth_failed');
        return;
      }
      const clientNonce = Buffer.from(auth.data.clientNonce, 'base64');
      const claimedHmac = Buffer.from(auth.data.hmac, 'base64');

      const expectedHmac = createHmac('sha256', Buffer.from(opts.hmacKey))
        .update(state.serverNonce)
        .update(clientNonce)
        .digest();

      let valid = false;
      if (claimedHmac.byteLength === expectedHmac.byteLength) {
        try {
          valid = timingSafeEqual(claimedHmac, expectedHmac);
        } catch {
          valid = false;
        }
      }
      if (!valid) {
        sendLine(state, { type: 'AUTH_DENY', reason: 'auth_failed' });
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'denied',
          details: { kind: 'auth_failed' },
        });
        dropConnection(state, 'auth_failed');
        return;
      }
      // Success — server returns its own HMAC over (clientNonce || serverNonce)
      // so the client can verify the server too.
      const reverseHmac = createHmac('sha256', Buffer.from(opts.hmacKey))
        .update(clientNonce)
        .update(state.serverNonce)
        .digest();
      sendLine(state, { type: 'AUTH_OK', hmac: reverseHmac.toString('base64') });
      state.phase = 'authenticated';
      return;
    }

    if (state.phase === 'authenticated') {
      await handleRequest(state, json.value);
      return;
    }
    // closed — ignore.
  }

  function onConnection(socket: Socket): void {
    // (1) peer credential check FIRST.
    const peer = peerUidMatchesOurs(socket);
    if (!peer.ok) {
      void audit({
        action: AuditActionConst.IpcKeybridgeRequestDenied,
        outcome: 'denied',
        details: { kind: 'peer_credential_mismatch', reason: peer.reason },
      });
      try {
        socket.destroy();
      } catch {
        // already destroyed
      }
      return;
    }

    const state: ConnectionState = {
      socket,
      splitter: createLineSplitter(),
      phase: 'awaiting_hello',
      serverNonce: null,
      invalidatedSessions: new Set(),
    };
    connections.add(state);

    const handshakeTimeout = opts.handshakeTimeoutMs ?? DEFAULT_HANDSHAKE_TIMEOUT_MS;
    const handshakeTimer = setTimeout(() => {
      if (state.phase !== 'authenticated' && state.phase !== 'closed') {
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'failure',
          details: { kind: 'handshake_timeout' },
        });
        dropConnection(state, 'handshake_timeout');
      }
    }, handshakeTimeout);
    handshakeTimer.unref();

    socket.on('data', (chunk: Buffer) => {
      const r = state.splitter.push(chunk);
      if (r.oversized) {
        void audit({
          action: AuditActionConst.IpcKeybridgeRequestDenied,
          outcome: 'failure',
          details: { kind: 'oversized_line' },
        });
        dropConnection(state, 'oversized');
        return;
      }
      void (async (): Promise<void> => {
        for (const line of r.lines) {
          if (state.phase === 'closed') {
            return;
          }
          await handleLine(state, line);
        }
      })();
    });

    socket.on('error', () => {
      dropConnection(state, 'socket_error');
    });
    socket.on('close', () => {
      clearTimeout(handshakeTimer);
      dropConnection(state, 'socket_close');
    });
  }

  return Object.freeze({
    async start(): Promise<Result<void, KeybridgeError>> {
      if (server !== null) {
        return Ok(undefined);
      }
      // Cleanup stale socket from a previous crash.
      try {
        // eslint-disable-next-line security/detect-non-literal-fs-filename -- socketPath comes from caller config, not user input
        fs.unlinkSync(opts.socketPath);
      } catch {
        // ENOENT is the expected case — fine.
      }
      const s = createServer(onConnection);
      try {
        await new Promise<void>((resolve, reject) => {
          s.once('error', reject);
          s.listen(opts.socketPath, () => {
            s.removeListener('error', reject);
            resolve();
          });
        });
        // Set 0600 immediately so other-uid peers cannot connect even between
        // bind and the next tick.
        // eslint-disable-next-line security/detect-non-literal-fs-filename -- socketPath comes from caller config, not user input
        fs.chmodSync(opts.socketPath, 0o600);
      } catch {
        try {
          s.close();
        } catch {
          // already closed
        }
        return Err({ kind: 'socket_unavailable' });
      }
      server = s;
      return Ok(undefined);
    },
    async stop(): Promise<void> {
      const s = server;
      server = null;
      if (s !== null) {
        await new Promise<void>((resolve) => {
          s.close(() => resolve());
        });
      }
      for (const c of connections) {
        dropConnection(c, 'server_stop');
      }
      try {
        // eslint-disable-next-line security/detect-non-literal-fs-filename -- socketPath comes from caller config, not user input
        fs.unlinkSync(opts.socketPath);
      } catch {
        // already gone
      }
    },
    async invalidateSession(sessionId: SessionId): Promise<void> {
      const sid = sessionId as unknown as string;
      for (const c of connections) {
        c.invalidatedSessions.add(sid);
      }
    },
    _testing: {
      serverInstance: (): Server | null => server,
    },
  });
}

/** Re-export for tests/boot. */
export type { KeybridgeServer } from '../types/services.js';

// Useful test seam: opaque request id factory.
export function makeRequestId(): string {
  return randomUUID();
}
