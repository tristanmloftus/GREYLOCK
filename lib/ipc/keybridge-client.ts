// Greylock — IPC keybridge client (sync-worker side)
// =============================================================================
// AGENT-SYNC (Phase 3). Connects to the web process's Unix socket, runs the
// HMAC handshake, and exposes a small request/response API.
//
// The client is owned by the sync worker. On every borrow:
//   1. requestDek({...}) → server returns DEK bytes inside a base64 string.
//   2. The client materializes a Buffer copy with the bytes.
//   3. Caller `use`s the Buffer for one item sync.
//   4. Caller calls `release()` which:
//      - sends `releaseDek` to the server (audit trail)
//      - `Buffer.fill(0)` on the local copy.
// =============================================================================

import type { Socket} from 'node:net';
import { connect } from 'node:net';
import { createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';

import { Err, Ok } from '../types/domain.js';
import type { Result } from '../types/domain.js';
import type { KeybridgeError } from '../types/services.js';

import {
  AuthOkSchema,
  AuthDenySchema,
  HelloOkSchema,
  ResponseSchema,
  createLineSplitter,
  decodeJson,
  encodeLine,
  PROTOCOL_VERSION,
} from './keybridge-protocol.js';

// -----------------------------------------------------------------------------
// Public types
// -----------------------------------------------------------------------------

export interface KeybridgeClientOptions {
  readonly socketPath: string;
  /** HMAC key derived from the same Master KEK at boot. */
  readonly hmacKey: Uint8Array;
  /** Default per-request timeout in ms. */
  readonly requestTimeoutMs?: number;
}

export interface BorrowedDek {
  readonly handle:
    | { readonly kind: 'pcc'; readonly version: number }
    | { readonly kind: 'user'; readonly userId: string; readonly version: number };
  /** Buffer holding the borrowed DEK bytes. Mutable so caller can zeroize via release(). */
  readonly bytes: Buffer;
  /** Idempotent: zeros the bytes and signals the server to drop the borrow. */
  release: () => Promise<void>;
}

export type KeybridgeClient = {
  connect(): Promise<Result<void, KeybridgeError>>;
  disconnect(): Promise<void>;
  isConnected(): boolean;
  requestDek(
    input:
      | { readonly kind: 'pcc' }
      | { readonly userId: string; readonly sessionId: string },
  ): Promise<Result<BorrowedDek, KeybridgeError>>;
  ping(): Promise<Result<void, KeybridgeError>>;
};

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

interface PendingRequest {
  readonly id: string;
  readonly resolve: (value: { ok: true; result?: unknown } | { ok: false; kind: KeybridgeError['kind'] }) => void;
  readonly timer: NodeJS.Timeout;
}

const DEFAULT_REQUEST_TIMEOUT_MS = 5_000;

export function createKeybridgeClient(opts: KeybridgeClientOptions): KeybridgeClient {
  let socket: Socket | null = null;
  let connected = false;
  let pending: Map<string, PendingRequest> = new Map();
  const splitter = createLineSplitter();

  function failAllPending(kind: KeybridgeError['kind']): void {
    for (const p of pending.values()) {
      clearTimeout(p.timer);
      p.resolve({ ok: false, kind });
    }
    pending = new Map();
  }

  function onLine(line: Buffer): void {
    const json = decodeJson(line);
    if (!json.ok) {
      failAllPending('protocol_error');
      teardown();
      return;
    }
    const parsed = ResponseSchema.safeParse(json.value);
    if (!parsed.success) {
      // Could be HELLO_OK / AUTH_OK during handshake; those are handled inline.
      // After handshake, anything else is a protocol error.
      failAllPending('protocol_error');
      teardown();
      return;
    }
    const resp = parsed.data;
    const p = pending.get(resp.id);
    if (p === undefined) {
      // Stray response — ignore.
      return;
    }
    pending.delete(resp.id);
    clearTimeout(p.timer);
    if (resp.ok) {
      p.resolve({ ok: true, result: resp.result });
    } else {
      const kind = resp.error?.kind ?? 'protocol_error';
      p.resolve({ ok: false, kind });
    }
  }

  function teardown(): void {
    connected = false;
    if (socket !== null) {
      try {
        socket.destroy();
      } catch {
        // already destroyed
      }
    }
    socket = null;
  }

  async function connectAndHandshake(): Promise<Result<void, KeybridgeError>> {
    return await new Promise<Result<void, KeybridgeError>>((resolve) => {
      let resolved = false;
      const finish = (r: Result<void, KeybridgeError>): void => {
        if (resolved) {
          return;
        }
        resolved = true;
        resolve(r);
      };

      let s: Socket;
      try {
        s = connect(opts.socketPath);
      } catch {
        finish(Err({ kind: 'socket_unavailable' }));
        return;
      }
      socket = s;

      const overallTimer = setTimeout(() => {
        teardown();
        finish(Err({ kind: 'timeout' }));
      }, opts.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS);
      overallTimer.unref();

      let handshakePhase: 'awaiting_hello_ok' | 'awaiting_auth_ok' | 'done' = 'awaiting_hello_ok';
      let myClientNonce: Buffer | null = null;
      let serverNonceBuf: Buffer | null = null;

      s.once('connect', () => {
        // Send HELLO.
        try {
          s.write(
            encodeLine({
              type: 'HELLO',
              v: PROTOCOL_VERSION,
              pid: typeof process.pid === 'number' ? process.pid : 0,
              uid: typeof process.getuid === 'function' ? process.getuid() : 0,
            }),
          );
        } catch {
          finish(Err({ kind: 'socket_unavailable' }));
        }
      });

      s.on('error', () => {
        clearTimeout(overallTimer);
        teardown();
        finish(Err({ kind: 'socket_unavailable' }));
      });

      s.on('close', () => {
        clearTimeout(overallTimer);
        if (handshakePhase !== 'done') {
          finish(Err({ kind: 'auth_failed' }));
        }
        connected = false;
        failAllPending('socket_unavailable');
      });

      s.on('data', (chunk: Buffer) => {
        const r = splitter.push(chunk);
        if (r.oversized) {
          clearTimeout(overallTimer);
          teardown();
          finish(Err({ kind: 'protocol_error' }));
          return;
        }
        for (const line of r.lines) {
          if (handshakePhase === 'awaiting_hello_ok') {
            const json = decodeJson(line);
            if (!json.ok) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'protocol_error' }));
              return;
            }
            const denied = AuthDenySchema.safeParse(json.value);
            if (denied.success) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'auth_failed' }));
              return;
            }
            const helloOk = HelloOkSchema.safeParse(json.value);
            if (!helloOk.success) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'protocol_error' }));
              return;
            }
            // Compute AUTH HMAC.
            const sNonce = Buffer.from(helloOk.data.serverNonce, 'base64');
            const cNonce = randomBytes(32);
            const myHmac = createHmac('sha256', Buffer.from(opts.hmacKey))
              .update(sNonce)
              .update(cNonce)
              .digest();
            myClientNonce = cNonce;
            serverNonceBuf = sNonce;
            try {
              s.write(
                encodeLine({
                  type: 'AUTH',
                  clientNonce: cNonce.toString('base64'),
                  hmac: myHmac.toString('base64'),
                }),
              );
            } catch {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'socket_unavailable' }));
              return;
            }
            handshakePhase = 'awaiting_auth_ok';
            continue;
          }
          if (handshakePhase === 'awaiting_auth_ok') {
            const json = decodeJson(line);
            if (!json.ok) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'protocol_error' }));
              return;
            }
            const denied = AuthDenySchema.safeParse(json.value);
            if (denied.success) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'auth_failed' }));
              return;
            }
            const authOk = AuthOkSchema.safeParse(json.value);
            if (!authOk.success || myClientNonce === null || serverNonceBuf === null) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'protocol_error' }));
              return;
            }
            const expected = createHmac('sha256', Buffer.from(opts.hmacKey))
              .update(myClientNonce)
              .update(serverNonceBuf)
              .digest();
            const got = Buffer.from(authOk.data.hmac, 'base64');
            let valid = false;
            if (got.byteLength === expected.byteLength) {
              try {
                valid = timingSafeEqual(got, expected);
              } catch {
                valid = false;
              }
            }
            if (!valid) {
              clearTimeout(overallTimer);
              teardown();
              finish(Err({ kind: 'auth_failed' }));
              return;
            }
            // Handshake complete.
            clearTimeout(overallTimer);
            handshakePhase = 'done';
            connected = true;
            finish(Ok(undefined));
            continue;
          }
          // Steady-state: dispatch responses.
          onLine(line);
        }
      });
    });
  }

  async function sendRequest(
    method: 'requestDek' | 'releaseDek' | 'ping',
    params: Readonly<Record<string, unknown>>,
  ): Promise<{ ok: true; result?: unknown } | { ok: false; kind: KeybridgeError['kind'] }> {
    if (!connected || socket === null) {
      return { ok: false, kind: 'socket_unavailable' };
    }
    const id = randomUUID();
    const promise = new Promise<{ ok: true; result?: unknown } | { ok: false; kind: KeybridgeError['kind'] }>(
      (resolve) => {
        const timer = setTimeout(() => {
          pending.delete(id);
          resolve({ ok: false, kind: 'timeout' });
        }, opts.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS);
        timer.unref();
        pending.set(id, { id, resolve, timer });
      },
    );
    try {
      socket.write(encodeLine({ type: 'REQUEST', id, method, params }));
    } catch {
      pending.delete(id);
      return { ok: false, kind: 'socket_unavailable' };
    }
    return await promise;
  }

  return Object.freeze({
    async connect(): Promise<Result<void, KeybridgeError>> {
      if (connected) {
        return Ok(undefined);
      }
      return await connectAndHandshake();
    },
    async disconnect(): Promise<void> {
      teardown();
    },
    isConnected(): boolean {
      return connected;
    },
    async requestDek(input): Promise<Result<BorrowedDek, KeybridgeError>> {
      const params: Readonly<Record<string, unknown>> =
        'kind' in input
          ? { kind: 'pcc' }
          : { userId: input.userId, sessionId: input.sessionId };
      const r = await sendRequest('requestDek', params);
      if (!r.ok) {
        return Err({ kind: r.kind });
      }
      const result = r.result as
        | undefined
        | {
            handle?: { kind?: string };
            dekB64?: string;
          };
      if (
        result === undefined ||
        result.handle === undefined ||
        typeof result.dekB64 !== 'string'
      ) {
        return Err({ kind: 'protocol_error' });
      }
      const bytes = Buffer.from(result.dekB64, 'base64');
      const handle = result.handle as BorrowedDek['handle'];
      const sendRelease = sendRequest;
      let released = false;
      const borrowed: BorrowedDek = {
        handle,
        bytes,
        release: async (): Promise<void> => {
          if (released) {
            return;
          }
          released = true;
          try {
            bytes.fill(0);
          } catch {
            // best-effort
          }
          if (connected) {
            await sendRelease('releaseDek', { handle }).catch(() => undefined);
          }
        },
      };
      return Ok(borrowed);
    },
    async ping(): Promise<Result<void, KeybridgeError>> {
      const r = await sendRequest('ping', {});
      return r.ok ? Ok(undefined) : Err({ kind: r.kind });
    },
  });
}
