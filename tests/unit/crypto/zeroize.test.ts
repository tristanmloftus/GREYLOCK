// Tests for `lib/crypto/zeroize.ts`.

import { describe, it, expect } from 'vitest';

import { withZeroized, zeroize } from '../../../lib/crypto/zeroize.js';

describe('zeroize', () => {
  it('overwrites all bytes with 0x00', () => {
    const buf = Buffer.from([1, 2, 3, 4, 5]);
    zeroize(buf);
    expect([...buf]).toEqual([0, 0, 0, 0, 0]);
  });

  it('is a no-op on a zero-length buffer', () => {
    const buf = Buffer.alloc(0);
    zeroize(buf); // must not throw
    expect(buf.byteLength).toBe(0);
  });

  it('handles a Uint8Array (non-Buffer view)', () => {
    const u8 = new Uint8Array([7, 8, 9]);
    zeroize(u8);
    expect([...u8]).toEqual([0, 0, 0]);
  });

  it('does not throw on a detached ArrayBuffer view', () => {
    const buf = Buffer.from([1, 2, 3]);
    // Force a detach via structuredClone-style transfer is not portable in
    // Node tests — instead, simulate by replacing fill to throw.
    const original = Buffer.prototype.fill;
    try {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      (Buffer.prototype as any).fill = function () {
        throw new Error('detached');
      };
      // Should not throw out of zeroize.
      expect(() => zeroize(buf)).not.toThrow();
    } finally {
      Buffer.prototype.fill = original;
    }
  });
});

describe('withZeroized', () => {
  it('zeroizes the buffer when useFn resolves', async () => {
    let captured: Uint8Array | null = null;
    await withZeroized(
      () => {
        const b = Buffer.from([1, 2, 3, 4]);
        captured = b;
        return b;
      },
      async (buf) => {
        expect([...buf]).toEqual([1, 2, 3, 4]);
      },
    );
    expect(captured).not.toBeNull();
    expect([...(captured as unknown as Buffer)]).toEqual([0, 0, 0, 0]);
  });

  it('zeroizes the buffer even if useFn rejects', async () => {
    let captured: Uint8Array | null = null;
    await expect(
      withZeroized(
        () => {
          const b = Buffer.from([9, 9, 9]);
          captured = b;
          return b;
        },
        async () => {
          throw new Error('boom');
        },
      ),
    ).rejects.toThrow('boom');
    expect(captured).not.toBeNull();
    expect([...(captured as unknown as Buffer)]).toEqual([0, 0, 0]);
  });

  it('zeroizes the buffer if useFn throws synchronously', async () => {
    let captured: Uint8Array | null = null;
    await expect(
      withZeroized(
        () => {
          const b = Buffer.from([5, 6]);
          captured = b;
          return b;
        },
        // sync throw inside async — translated to rejected promise
        async () => {
          throw new RangeError('nope');
        },
      ),
    ).rejects.toBeInstanceOf(RangeError);
    expect([...(captured as unknown as Buffer)]).toEqual([0, 0]);
  });

  it('returns the value useFn produces', async () => {
    const result = await withZeroized(
      () => Buffer.alloc(4, 0xff),
      async () => 'ok-value',
    );
    expect(result).toBe('ok-value');
  });
});
