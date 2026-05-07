// Greylock — Master KEK
// =============================================================================
// (1) Fetch master passphrase from macOS Keychain (with optional TTY fallback).
// (2) Derive Master KEK = scrypt(passphrase || CRYPTO_PEPPER_BYTES, kdfSalt,
//     N=2^17, r=8, p=1, dkLen=32).
//
// SECURITY contract for this module:
//   - The master passphrase is NEVER returned across the public boundary.
//     It exists only inside `withPassphraseBytes` and is `Buffer.fill(0)`'d
//     in a `finally`.
//   - The string 'passphrase' MUST NOT appear in any thrown / returned error
//     message. (See `MasterKeyFailure` below.)
//   - This module never logs.
//   - The exec'd command is a fixed argv (no shell interpolation).
// =============================================================================

import { spawn } from 'node:child_process';
import type { EventEmitter } from 'node:events';
import type { Readable } from 'node:stream';

import { SCRYPT_PARAMS, scrypt } from './kdf.js';
import { withZeroized, zeroize } from './zeroize.js';

export type MasterKeyFailure =
  | { readonly kind: 'master_passphrase_unavailable'; readonly reason: 'keychain_missing' | 'keychain_error' | 'fallback_disabled' | 'tty_unavailable' }
  | { readonly kind: 'kdf_failure' };

/**
 * Minimal child-process shape this module needs from a spawn implementation.
 * Defining it locally (rather than reusing `typeof spawn` from node:child_process)
 * avoids `exactOptionalPropertyTypes` and Node's overload-soup typings making
 * the test seam impossible to satisfy.
 */
export interface MinimalChildProcess extends EventEmitter {
  readonly stdout: Readable | null;
  readonly stderr: Readable | null;
}

export type SpawnImpl = (
  command: string,
  args: ReadonlyArray<string>,
  options: { readonly stdio: ['ignore', 'pipe', 'pipe'] },
) => MinimalChildProcess;

export interface KeychainFetchOptions {
  /** Service name for `security find-generic-password -s <name>`. Default: `greylock-master`. */
  readonly serviceName?: string;
  /** Account name. Default: `process.env.USER`. */
  readonly accountName?: string;
  /** When true (and KEYCHAIN_FALLBACK_TTY=true), prompt on TTY if Keychain item missing. */
  readonly fallbackTty?: boolean;
  /** Test seam: override the spawn function. Production code passes nothing. */
  readonly spawnImpl?: SpawnImpl;
  /** Test seam: override the TTY prompt. Production code passes nothing. */
  readonly ttyPromptImpl?: () => Promise<Buffer>;
}

const DEFAULT_SERVICE_NAME = 'greylock-master';

/**
 * Run a function with the master-passphrase bytes loaded into a Buffer.
 * The buffer is zeroized in `finally`. The bytes never leave this scope.
 *
 * On Keychain miss + `fallbackTty=true`, attempts a TTY prompt (silent, no
 * echo). If neither path succeeds the failure is surfaced — without ever
 * including the literal string "passphrase" or any byte of any captured
 * value in the error.
 *
 * NOTE: this function is exported for use by `pcc-dek.ts` and tests. It is
 * NOT exported from `lib/crypto/index.ts` — the surface that other modules
 * see is `CryptoService`, never raw passphrase access.
 */
export async function withPassphraseBytes<T>(
  options: KeychainFetchOptions,
  use: (buf: Buffer) => Promise<T>,
): Promise<T | { readonly _failure: MasterKeyFailure }> {
  const result = await readMasterSecretBytes(options);
  if ('_failure' in result) {
    return result;
  }
  const buf = result.bytes;
  try {
    return await use(buf);
  } finally {
    zeroize(buf);
  }
}

interface OkBytes {
  readonly bytes: Buffer;
}

async function readMasterSecretBytes(
  options: KeychainFetchOptions,
): Promise<OkBytes | { readonly _failure: MasterKeyFailure }> {
  const fallbackTty = options.fallbackTty === true;
  const fromKeychain = await tryKeychain(options);
  if (fromKeychain.ok) {
    return { bytes: fromKeychain.bytes };
  }
  if (fromKeychain.fatal) {
    return { _failure: { kind: 'master_passphrase_unavailable', reason: 'keychain_error' } };
  }
  // Item missing.
  if (!fallbackTty) {
    return { _failure: { kind: 'master_passphrase_unavailable', reason: 'fallback_disabled' } };
  }
  const fromTty = await tryTtyPrompt(options);
  if (fromTty.ok) {
    return { bytes: fromTty.bytes };
  }
  return { _failure: { kind: 'master_passphrase_unavailable', reason: fromTty.reason } };
}

interface KeychainResult {
  readonly ok: boolean;
  readonly fatal: boolean;
  readonly bytes: Buffer;
}

function tryKeychain(options: KeychainFetchOptions): Promise<KeychainResult> {
  const service = options.serviceName ?? DEFAULT_SERVICE_NAME;
  const account = options.accountName ?? process.env['USER'] ?? '';
  // Cast Node's overloaded `spawn` to our locally-defined minimal shape.
  // Node's spawn with `stdio: ['ignore','pipe','pipe']` returns a process
  // whose stdout/stderr are non-null Readable streams; the wider declared
  // shape (with `null`) is preserved for test stubs.
  const spawnFn: SpawnImpl =
    options.spawnImpl ?? ((cmd, args, opts) => spawn(cmd, [...args], opts) as unknown as MinimalChildProcess);

  return new Promise<KeychainResult>((resolve) => {
    let proc: MinimalChildProcess;
    try {
      proc = spawnFn('security', ['find-generic-password', '-s', service, '-a', account, '-w'], {
        stdio: ['ignore', 'pipe', 'pipe'],
      });
    } catch {
      resolve({ ok: false, fatal: true, bytes: Buffer.alloc(0) });
      return;
    }
    const chunks: Buffer[] = [];
    proc.stdout?.on('data', (chunk: Buffer) => {
      chunks.push(Buffer.from(chunk));
    });
    proc.stderr?.on('data', () => {
      // Discard stderr; never log it.
    });
    proc.on('error', () => {
      // ENOENT (security CLI missing) => fatal
      resolve({ ok: false, fatal: true, bytes: Buffer.alloc(0) });
    });
    proc.on('close', (code: number | null) => {
      if (code === 0) {
        const joined = Buffer.concat(chunks);
        // `security ... -w` prints the secret followed by a single newline.
        const trimmed = stripTrailingNewline(joined);
        // zeroize the intermediate `joined` if we made a fresh allocation
        if (trimmed !== joined) {
          zeroize(joined);
        }
        resolve({ ok: true, fatal: false, bytes: trimmed });
        return;
      }
      // Exit code 44 = item not found (the typical "missing" path).
      // Any other non-zero exit code we treat as fatal/error.
      const fatal = code !== 44;
      // Discard any captured bytes in the error path.
      for (const c of chunks) {
        zeroize(c);
      }
      resolve({ ok: false, fatal, bytes: Buffer.alloc(0) });
    });
  });
}

function stripTrailingNewline(buf: Buffer): Buffer {
  if (buf.byteLength === 0) {
    return buf;
  }
  if (buf[buf.byteLength - 1] === 0x0a) {
    return Buffer.from(buf.subarray(0, buf.byteLength - 1));
  }
  return buf;
}

interface TtyResult {
  readonly ok: boolean;
  readonly bytes: Buffer;
  readonly reason: 'tty_unavailable';
}

async function tryTtyPrompt(options: KeychainFetchOptions): Promise<TtyResult> {
  if (options.ttyPromptImpl !== undefined) {
    try {
      const bytes = await options.ttyPromptImpl();
      if (bytes.byteLength === 0) {
        return { ok: false, bytes: Buffer.alloc(0), reason: 'tty_unavailable' };
      }
      return { ok: true, bytes, reason: 'tty_unavailable' };
    } catch {
      return { ok: false, bytes: Buffer.alloc(0), reason: 'tty_unavailable' };
    }
  }
  // We deliberately do NOT bundle a real TTY-prompt implementation here.
  // The web process boot path in `lib/runtime/boot.ts` is responsible for
  // wiring a TTY prompt (e.g. via `readline` with stdin echo off) when it
  // wants the fallback. This module remains side-effect-free.
  if (process.stdin.isTTY !== true) {
    return { ok: false, bytes: Buffer.alloc(0), reason: 'tty_unavailable' };
  }
  return { ok: false, bytes: Buffer.alloc(0), reason: 'tty_unavailable' };
}

// -----------------------------------------------------------------------------
// Master KEK derivation
// -----------------------------------------------------------------------------

export interface DeriveMasterKekInput {
  /** Master-secret bytes (already loaded into a Buffer). Caller owns lifecycle. */
  readonly secretBytes: Uint8Array;
  /** Per-wrap salt from `PccKeyWrap.kdfSalt`. */
  readonly kdfSalt: Uint8Array;
  /** Bytes of `CRYPTO_PEPPER` env var. Caller decodes the env var (typically base64). */
  readonly pepperBytes: Uint8Array;
  /** scrypt params, defaults to SCRYPT_PARAMS. */
  readonly N?: number;
  readonly r?: number;
  readonly p?: number;
}

/**
 * Derive a 32-byte Master KEK from a master secret and per-wrap salt.
 *
 * The salt passed to scrypt is `kdfSalt || pepperBytes` per ARCHITECTURE.md §3.
 * Returns a fresh Buffer; caller must zeroize after use.
 *
 * Throws (caller catches at boundary) on KDF failure.
 */
export function deriveMasterKek(input: DeriveMasterKekInput): Buffer {
  const N = input.N ?? SCRYPT_PARAMS.N;
  const r = input.r ?? SCRYPT_PARAMS.r;
  const p = input.p ?? SCRYPT_PARAMS.p;
  const saltMixed = Buffer.concat([
    Buffer.from(input.kdfSalt),
    Buffer.from(input.pepperBytes),
  ]);
  try {
    return scrypt({
      password: input.secretBytes,
      salt: saltMixed,
      N,
      r,
      p,
      length: 32,
    });
  } finally {
    zeroize(saltMixed);
  }
}

/**
 * Convenience: load secret from Keychain, derive Master KEK in one call,
 * zeroize the secret buffer, and pass the KEK to `use`. The KEK is NOT
 * zeroized automatically here — the caller manages its lifetime in module-
 * private state.
 *
 * If the secret cannot be obtained, returns the failure shape directly.
 */
export async function loadMasterKek(
  options: KeychainFetchOptions,
  kdfSalt: Uint8Array,
  pepperBytes: Uint8Array,
  scryptOverride?: { readonly N?: number; readonly r?: number; readonly p?: number },
): Promise<{ readonly ok: true; readonly kek: Buffer } | { readonly ok: false; readonly error: MasterKeyFailure }> {
  const wrapped = await withPassphraseBytes(options, async (secret) => {
    return withZeroized(
      () => Buffer.alloc(32),
      async (kekTmp) => {
        const derived = deriveMasterKek({
          secretBytes: secret,
          kdfSalt,
          pepperBytes,
          ...(scryptOverride?.N !== undefined ? { N: scryptOverride.N } : {}),
          ...(scryptOverride?.r !== undefined ? { r: scryptOverride.r } : {}),
          ...(scryptOverride?.p !== undefined ? { p: scryptOverride.p } : {}),
        });
        // Copy into the protected return buffer so we can zero-fill `derived`.
        // Both copies hold the same KEK; the caller receives a fresh allocation.
        derived.copy(kekTmp);
        zeroize(derived);
        return Buffer.from(kekTmp);
      },
    );
  });
  if (typeof wrapped === 'object' && wrapped !== null && '_failure' in wrapped) {
    return { ok: false, error: wrapped._failure };
  }
  return { ok: true, kek: wrapped };
}
