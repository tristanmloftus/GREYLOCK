// Greylock — buffer zeroization helpers
// =============================================================================
// Best-effort overwrite of buffers that held key/DEK/KEK/token plaintext.
// V8/libuv may keep internal copies we cannot reach (THREAT_MODEL.md §1.4 D-8);
// these helpers do what user-space Node *can* do.
//
// IMPORTANT: this module never throws across its public boundary, never logs,
// and never inspects buffer contents. It only writes zeros and returns.
// =============================================================================

/**
 * Overwrite the underlying memory of `buf` with 0x00 bytes.
 *
 * For `Buffer` and `Uint8Array` this calls `.fill(0)` which writes through
 * to the underlying ArrayBuffer view. After this call the bytes the caller
 * was protecting are gone from this view — though V8 may have made copies
 * during JIT or string interning that we cannot reach.
 *
 * No-op on a zero-length buffer. Detached buffers throw inside `.fill` —
 * we treat that as already-cleared and ignore it.
 */
export function zeroize(buf: Uint8Array): void {
  if (buf.byteLength === 0) {
    return;
  }
  try {
    buf.fill(0);
  } catch {
    // ArrayBuffer was already detached / transferred. Nothing more we can do.
    // We deliberately swallow this — surfacing it would mean exposing the
    // existence of a key buffer to an error handler, which violates the
    // "never log key state" rule.
  }
}

/**
 * Allocate a buffer via `allocFn`, pass it to `useFn`, and guarantee `zeroize`
 * runs even if `useFn` rejects or throws synchronously.
 *
 * Use this around any block that holds a DEK/KEK/token in a Buffer for the
 * duration of one logical operation:
 *
 *   const result = await withZeroized(
 *     () => Buffer.alloc(32),
 *     async (buf) => {
 *       // ... use buf as a key ...
 *       return doThing(buf);
 *     },
 *   );
 *
 * Contract:
 *   - `allocFn` runs first. If it throws, no zeroize is needed (no buffer).
 *   - `useFn` runs with the allocated buffer. Whether it resolves or rejects,
 *     `zeroize(buf)` runs in a `finally`.
 *   - The original resolution / rejection is preserved verbatim.
 */
export async function withZeroized<T>(
  allocFn: () => Uint8Array,
  useFn: (buf: Uint8Array) => Promise<T>,
): Promise<T> {
  const buf = allocFn();
  try {
    return await useFn(buf);
  } finally {
    zeroize(buf);
  }
}
