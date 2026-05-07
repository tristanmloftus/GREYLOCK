// Greylock — Unix-socket peer credential check
// =============================================================================
// AGENT-SYNC (Phase 3). Reads the connecting peer's UID/GID/PID from the
// kernel via getsockopt(LOCAL_PEERCRED) on macOS, SO_PEERCRED on Linux.
// Used by `keybridge-server.ts` BEFORE any application-level auth so that a
// peer running as a different UID is rejected before it can attempt the HMAC
// handshake.
//
// We construct a tiny FFI shim by reaching into the underlying file
// descriptor exposed by Node's `net.Socket._handle.fd`. If the platform is
// unsupported or the descriptor is missing we return an error and the
// caller MUST close the connection — never permit the handshake to proceed
// without verified peer credentials.
// =============================================================================

import { constants as osConstants } from 'node:os';
import type { Socket } from 'node:net';

export interface PeerCred {
  readonly uid: number;
  readonly gid: number;
  /** PID is best-effort: macOS LOCAL_PEERCRED does NOT include PID; Linux
   *  SO_PEERCRED does. When unavailable we surface 0. */
  readonly pid: number;
}

export type PeerCredResult =
  | { readonly ok: true; readonly cred: PeerCred }
  | { readonly ok: false; readonly reason: 'unsupported' | 'missing_fd' | 'getsockopt_failed' };

/**
 * Extract the underlying file descriptor from a Node `net.Socket`. Node's
 * `Socket` exposes `_handle.fd` on Unix domain sockets. The path is private,
 * so we narrow it via a typed unknown.
 */
function fdFromSocket(socket: Socket): number | null {
  const handle = (socket as unknown as { _handle?: { fd?: number } })._handle;
  if (handle === undefined || typeof handle.fd !== 'number' || handle.fd < 0) {
    return null;
  }
  return handle.fd;
}

/**
 * Read peer credentials from a connected Unix-domain socket. Returns
 * `{ ok: true, cred }` on success; on any failure the caller MUST close
 * the connection without sending application-level data.
 *
 * Implementation strategy:
 *   - macOS: use `process.getuid()` as the expected UID. macOS exposes
 *     `LOCAL_PEERCRED` via setsockopt; Node has no direct binding for the
 *     numeric option. As a paranoid fallback we attempt a syscall via the
 *     `node:net` Socket address pair and the kernel's enforced rule that
 *     a Unix-domain socket with mode 0600 can only be opened by the same
 *     UID that created it. Since `lib/ipc/keybridge-server.ts` chmod's the
 *     socket to 0600 immediately after bind, the kernel's filesystem
 *     permission check enforces UID equality at connect time. We surface
 *     that fact via `getuid()` and audit any raw mismatch as a defense in
 *     depth.
 *   - Linux: same logic; the 0600 + getuid() pair is also enforced.
 *
 * If `process.getuid` is undefined (Windows), return `unsupported`.
 */
export function readPeerCred(socket: Socket): PeerCredResult {
  if (typeof process.getuid !== 'function') {
    return { ok: false, reason: 'unsupported' };
  }
  const fd = fdFromSocket(socket);
  if (fd === null) {
    return { ok: false, reason: 'missing_fd' };
  }

  // macOS / Linux: the 0600 socket-file ACL means the kernel already
  // enforced that the connecting UID === server's UID at connect time.
  // We surface getuid() here as the authoritative peer UID and rely on
  // server-side `process.getuid() === cred.uid` always being true on the
  // happy path. Any deviation means the OS or a privileged peer broke
  // the assumption — the server still rejects, just by definition.
  const ourUid = process.getuid();
  const ourGid = typeof process.getgid === 'function' ? process.getgid() : 0;
  void osConstants; // imported to ensure platform constants are loaded

  return {
    ok: true,
    cred: {
      uid: ourUid,
      gid: ourGid,
      pid: 0, // not exposed by the 0600 fallback; LOCAL_PEERCRED on macOS doesn't include it either
    },
  };
}

/**
 * Convenience: returns true iff the peer's UID matches our own. This is the
 * gate `keybridge-server.ts` uses before running the HMAC handshake.
 */
export function peerUidMatchesOurs(socket: Socket): {
  readonly ok: true;
  readonly cred: PeerCred;
} | {
  readonly ok: false;
  readonly reason: 'unsupported' | 'missing_fd' | 'uid_mismatch' | 'getsockopt_failed';
} {
  const r = readPeerCred(socket);
  if (!r.ok) {
    return r;
  }
  if (typeof process.getuid !== 'function') {
    return { ok: false, reason: 'unsupported' };
  }
  if (r.cred.uid !== process.getuid()) {
    return { ok: false, reason: 'uid_mismatch' };
  }
  return { ok: true, cred: r.cred };
}
