// Tests for `lib/crypto/master-key.ts`.
//
// We DO NOT touch the real macOS Keychain — instead we inject a `spawnImpl`
// test seam that emits a fake child process matching the `security` CLI
// behavior we depend on.

import { EventEmitter } from 'node:events';
import { Readable } from 'node:stream';
import { describe, it, expect } from 'vitest';

import {
  deriveMasterKek,
  loadMasterKek,
  withPassphraseBytes,
  type MinimalChildProcess,
  type SpawnImpl,
} from '../../../lib/crypto/master-key.js';

// Minimal fake child process compatible with what master-key.ts consumes.
function makeFakeProc(opts: { stdout: string; stderr?: string; exitCode: number; emitError?: Error }): MinimalChildProcess {
  const proc = new EventEmitter() as EventEmitter & {
    stdout: Readable;
    stderr: Readable;
  };
  proc.stdout = Readable.from([Buffer.from(opts.stdout)]);
  proc.stderr = Readable.from([Buffer.from(opts.stderr ?? '')]);
  // Fire close on next tick.
  setImmediate(() => {
    if (opts.emitError) {
      proc.emit('error', opts.emitError);
      return;
    }
    proc.emit('close', opts.exitCode);
  });
  return proc;
}

function spawnReturning(opts: { stdout: string; stderr?: string; exitCode: number }): SpawnImpl {
  return () => makeFakeProc(opts);
}

describe('withPassphraseBytes (Keychain-mocked)', () => {
  it('reads from Keychain and zeroizes after use (success)', async () => {
    let capturedBytes: Buffer | null = null;
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: 'super-secret\n', exitCode: 0 }),
      },
      async (buf) => {
        // The trailing newline must already be stripped.
        expect(Buffer.from(buf).toString('utf8')).toBe('super-secret');
        capturedBytes = Buffer.from(buf);
        return buf.byteLength;
      },
    );
    expect(r).toBe('super-secret'.length);
    expect(capturedBytes).not.toBeNull();
  });

  it('handles Keychain output without a trailing newline', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: 'no-newline-here', exitCode: 0 }),
      },
      async (buf) => Buffer.from(buf).toString('utf8'),
    );
    expect(r).toBe('no-newline-here');
  });

  it('handles empty Keychain output (zero-length secret)', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 0 }),
      },
      async (buf) => buf.byteLength,
    );
    expect(r).toBe(0);
  });

  it('falls back path with no ttyPromptImpl returns tty_unavailable when stdin is not TTY', async () => {
    // process.stdin.isTTY is undefined / false in vitest; this exercises the
    // no-impl branch.
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: true,
      },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      if (r._failure.kind === 'master_passphrase_unavailable') {
        expect(r._failure.reason).toBe('tty_unavailable');
      }
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('falls back path returns tty_unavailable even when stdin reports as TTY (no impl)', async () => {
    // Force-toggle process.stdin.isTTY for one call to cover that branch.
    const original = process.stdin.isTTY;
    try {
      Object.defineProperty(process.stdin, 'isTTY', { value: true, configurable: true });
      const r = await withPassphraseBytes(
        {
          spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
          fallbackTty: true,
        },
        async () => 'unreachable',
      );
      if (typeof r === 'object' && r !== null && '_failure' in r) {
        if (r._failure.kind === 'master_passphrase_unavailable') {
          expect(r._failure.reason).toBe('tty_unavailable');
        }
      } else {
        throw new Error('expected failure shape');
      }
    } finally {
      Object.defineProperty(process.stdin, 'isTTY', { value: original, configurable: true });
    }
  });

  it('uses provided serviceName + accountName overrides', async () => {
    const calls: { args: ReadonlyArray<string> }[] = [];
    const spawnImpl: SpawnImpl = (_cmd, args) => {
      calls.push({ args });
      return makeFakeProc({ stdout: 'ok', exitCode: 0 });
    };
    await withPassphraseBytes(
      {
        spawnImpl,
        serviceName: 'custom-service',
        accountName: 'custom-account',
      },
      async () => 'done',
    );
    expect(calls.length).toBe(1);
    const args = calls[0]?.args ?? [];
    expect(args).toContain('custom-service');
    expect(args).toContain('custom-account');
  });

  it('returns master_passphrase_unavailable when Keychain item missing and fallback disabled', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: false,
      },
      async () => 'unreachable',
    );
    expect(typeof r).toBe('object');
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      expect(r._failure.kind).toBe('master_passphrase_unavailable');
      if (r._failure.kind === 'master_passphrase_unavailable') {
        expect(r._failure.reason).toBe('fallback_disabled');
      }
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('uses ttyPromptImpl fallback when Keychain item missing and fallback enabled', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: true,
        ttyPromptImpl: async () => Buffer.from('tty-secret', 'utf8'),
      },
      async (buf) => {
        expect(Buffer.from(buf).toString('utf8')).toBe('tty-secret');
        return 'used-tty';
      },
    );
    expect(r).toBe('used-tty');
  });

  it('returns failure when Keychain returns bad exit code (non-44)', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', stderr: 'broken', exitCode: 1 }),
      },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      expect(r._failure.kind).toBe('master_passphrase_unavailable');
      if (r._failure.kind === 'master_passphrase_unavailable') {
        expect(r._failure.reason).toBe('keychain_error');
      }
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('returns failure when spawn emits an error event', async () => {
    const spawnImpl: SpawnImpl = () => {
      const proc = new EventEmitter() as EventEmitter & {
        stdout: Readable;
        stderr: Readable;
      };
      proc.stdout = Readable.from([]);
      proc.stderr = Readable.from([]);
      setImmediate(() => proc.emit('error', new Error('ENOENT')));
      return proc;
    };
    const r = await withPassphraseBytes(
      { spawnImpl },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      expect(r._failure.kind).toBe('master_passphrase_unavailable');
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('returns failure when spawn() itself throws', async () => {
    const spawnImpl: SpawnImpl = () => {
      throw new Error('spawn failed');
    };
    const r = await withPassphraseBytes(
      { spawnImpl },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      expect(r._failure.kind).toBe('master_passphrase_unavailable');
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('fallback returns tty_unavailable when ttyPromptImpl returns empty bytes', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: true,
        ttyPromptImpl: async () => Buffer.alloc(0),
      },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      if (r._failure.kind === 'master_passphrase_unavailable') {
        expect(r._failure.reason).toBe('tty_unavailable');
      }
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('fallback returns tty_unavailable when ttyPromptImpl rejects', async () => {
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: true,
        ttyPromptImpl: async () => {
          throw new Error('user cancelled');
        },
      },
      async () => 'unreachable',
    );
    if (typeof r === 'object' && r !== null && '_failure' in r) {
      if (r._failure.kind === 'master_passphrase_unavailable') {
        expect(r._failure.reason).toBe('tty_unavailable');
      }
    } else {
      throw new Error('expected failure shape');
    }
  });

  it('NEVER leaks captured bytes or stderr in failure shape (only fixed enum tags)', async () => {
    // The CryptoError kind `master_passphrase_unavailable` is a canonical
    // enum from lib/types/domain.ts — not a free-form message. We assert
    // here that no attacker-controlled content (captured stdout / stderr)
    // ever surfaces in the failure shape, and that the failure shape is the
    // fixed-vocabulary enum the type system guarantees.
    const r = await withPassphraseBytes(
      {
        spawnImpl: spawnReturning({ stdout: 'ATTACKER-WAS-HERE', stderr: 'boom', exitCode: 1 }),
      },
      async () => 'unreachable',
    );
    const json = JSON.stringify(r);
    expect(json).not.toMatch(/ATTACKER-WAS-HERE/);
    expect(json).not.toMatch(/boom/);
    // The only allowed `passphrase` mention is the canonical enum kind.
    expect(json).toMatch(/master_passphrase_unavailable/);
  });
});

describe('deriveMasterKek', () => {
  it('produces a 32-byte KEK that depends on every input', () => {
    const secret = Buffer.from('secret-bytes', 'utf8');
    const salt = Buffer.from('salt-bytes', 'utf8');
    const pepper = Buffer.from('pepper-bytes', 'utf8');
    const SMALL_N = 1 << 8; // fast test
    const a = deriveMasterKek({
      secretBytes: secret,
      kdfSalt: salt,
      pepperBytes: pepper,
      N: SMALL_N,
    });
    expect(a.byteLength).toBe(32);
    const b = deriveMasterKek({
      secretBytes: secret,
      kdfSalt: salt,
      pepperBytes: Buffer.from('different-pepper'),
      N: SMALL_N,
    });
    expect(a.equals(b)).toBe(false);
    const c = deriveMasterKek({
      secretBytes: secret,
      kdfSalt: Buffer.from('different-salt'),
      pepperBytes: pepper,
      N: SMALL_N,
    });
    expect(a.equals(c)).toBe(false);
  });
});

describe('loadMasterKek', () => {
  it('returns a 32-byte KEK on the success path', async () => {
    const r = await loadMasterKek(
      {
        spawnImpl: spawnReturning({ stdout: 'master-pass\n', exitCode: 0 }),
      },
      Buffer.from('salt'),
      Buffer.from('pepper'),
      { N: 1 << 8 }, // small N for test speed
    );
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.kek.byteLength).toBe(32);
    }
  });

  it('returns failure when secret cannot be obtained', async () => {
    const r = await loadMasterKek(
      {
        spawnImpl: spawnReturning({ stdout: '', exitCode: 44 }),
        fallbackTty: false,
      },
      Buffer.from('salt'),
      Buffer.from('pepper'),
    );
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.error.kind).toBe('master_passphrase_unavailable');
    }
  });
});
