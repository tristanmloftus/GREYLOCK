// Tests for `lib/ipc/keybridge-protocol.ts`.
// Wire-format: encode/decode round-trip, oversized rejection, malformed JSON.

import { describe, it, expect } from 'vitest';

import {
  AnyMessage,
  AuthDenySchema,
  AuthOkSchema,
  AuthSchema,
  HelloOkSchema,
  HelloSchema,
  MAX_LINE_BYTES,
  PROTOCOL_VERSION,
  RequestSchema,
  ResponseSchema,
  createLineSplitter,
  decodeJson,
  decodeMessage,
  encodeLine,
} from '../../../lib/ipc/keybridge-protocol.js';

const b64 = (n: number, fill = 0): string => Buffer.alloc(n, fill).toString('base64');

describe('encodeLine / decodeMessage round-trip', () => {
  it('encodes a HELLO and decodes back to the same message', () => {
    const msg = { type: 'HELLO' as const, v: PROTOCOL_VERSION as 1, pid: 1234, uid: 501 };
    const encoded = encodeLine(msg);
    expect(encoded[encoded.byteLength - 1]).toBe(0x0a);
    const decoded = decodeMessage(encoded.subarray(0, encoded.byteLength - 1));
    expect(decoded.ok).toBe(true);
    if (decoded.ok) {
      expect(decoded.value.type).toBe('HELLO');
    }
  });

  it('encodes a HELLO_OK and decodes', () => {
    const msg = {
      type: 'HELLO_OK' as const,
      v: PROTOCOL_VERSION as 1,
      serverNonce: b64(32, 0xaa),
    };
    const decoded = decodeMessage(encodeLine(msg).subarray(0, -1));
    expect(decoded.ok).toBe(true);
    if (decoded.ok && decoded.value.type === 'HELLO_OK') {
      expect(decoded.value.serverNonce).toBe(msg.serverNonce);
    }
  });

  it('rejects HELLO_OK with wrong serverNonce length', () => {
    const json = JSON.stringify({
      type: 'HELLO_OK',
      v: 1,
      serverNonce: b64(31), // wrong length
    });
    const decoded = decodeMessage(json);
    expect(decoded.ok).toBe(false);
    if (!decoded.ok) {
      expect(decoded.kind).toBe('schema');
    }
  });

  it('round-trips an AUTH', () => {
    const msg = {
      type: 'AUTH' as const,
      clientNonce: b64(32),
      hmac: b64(32, 0x42),
    };
    const dec = decodeMessage(encodeLine(msg).subarray(0, -1));
    expect(dec.ok).toBe(true);
    if (dec.ok && dec.value.type === 'AUTH') {
      expect(dec.value.hmac).toBe(msg.hmac);
    }
  });

  it('round-trips an AUTH_OK and an AUTH_DENY', () => {
    const ok = decodeMessage(
      encodeLine({ type: 'AUTH_OK', hmac: b64(32) }).subarray(0, -1),
    );
    expect(ok.ok).toBe(true);
    if (ok.ok) {
      expect(ok.value.type).toBe('AUTH_OK');
    }
    const deny = decodeMessage(
      encodeLine({ type: 'AUTH_DENY', reason: 'auth_failed' }).subarray(0, -1),
    );
    expect(deny.ok).toBe(true);
    if (deny.ok) {
      expect(deny.value.type).toBe('AUTH_DENY');
    }
  });

  it('round-trips a REQUEST with method=requestDek', () => {
    const msg = {
      type: 'REQUEST' as const,
      id: 'req-1',
      method: 'requestDek' as const,
      params: { kind: 'pcc' },
    };
    const dec = decodeMessage(encodeLine(msg).subarray(0, -1));
    expect(dec.ok).toBe(true);
    if (dec.ok && dec.value.type === 'REQUEST') {
      expect(dec.value.method).toBe('requestDek');
    }
  });

  it('round-trips a RESPONSE ok', () => {
    const msg = {
      type: 'RESPONSE' as const,
      id: 'req-1',
      ok: true,
      result: { handle: { kind: 'pcc', version: 3 }, dekB64: b64(32) },
    };
    const dec = decodeMessage(encodeLine(msg).subarray(0, -1));
    expect(dec.ok).toBe(true);
    if (dec.ok && dec.value.type === 'RESPONSE') {
      expect(dec.value.ok).toBe(true);
    }
  });

  it('round-trips a RESPONSE error', () => {
    const dec = decodeMessage(
      encodeLine({
        type: 'RESPONSE',
        id: 'req-1',
        ok: false,
        error: { kind: 'session_invalid' },
      }).subarray(0, -1),
    );
    expect(dec.ok).toBe(true);
    if (dec.ok && dec.value.type === 'RESPONSE' && !dec.value.ok && dec.value.error !== undefined) {
      expect(dec.value.error.kind).toBe('session_invalid');
    }
  });
});

describe('decodeJson rejection paths', () => {
  it('rejects oversized input', () => {
    const big = Buffer.alloc(MAX_LINE_BYTES + 1, 0x61);
    const dec = decodeJson(big);
    expect(dec.ok).toBe(false);
    if (!dec.ok) {
      expect(dec.kind).toBe('oversized');
    }
  });

  it('rejects malformed JSON', () => {
    const dec = decodeJson('{not-json');
    expect(dec.ok).toBe(false);
    if (!dec.ok) {
      expect(dec.kind).toBe('malformed_json');
    }
  });

  it('rejects empty / whitespace-only lines', () => {
    expect(decodeJson('').ok).toBe(false);
    expect(decodeJson('   \t  ').ok).toBe(false);
  });
});

describe('encodeLine size guard', () => {
  it('throws when encoded line exceeds MAX_LINE_BYTES', () => {
    const huge = { type: 'AUTH_DENY', reason: 'x'.repeat(MAX_LINE_BYTES + 100) };
    expect(() => encodeLine(huge)).toThrow(/MAX_LINE_BYTES/);
  });
});

describe('createLineSplitter', () => {
  it('emits complete lines and buffers partial', () => {
    const sp = createLineSplitter();
    const a = sp.push(Buffer.from('a\nb', 'utf8'));
    expect(a.lines.map((b) => b.toString('utf8'))).toEqual(['a']);
    expect(a.oversized).toBe(false);
    const b = sp.push(Buffer.from('\nc\n', 'utf8'));
    expect(b.lines.map((x) => x.toString('utf8'))).toEqual(['b', 'c']);
  });

  it('flags oversized when a single line exceeds the cap', () => {
    const sp = createLineSplitter();
    const big = Buffer.alloc(MAX_LINE_BYTES + 1, 0x61); // no newline at all
    const r = sp.push(big);
    expect(r.oversized).toBe(true);
  });

  it('flags oversized when a terminated line exceeds the cap', () => {
    const sp = createLineSplitter();
    const big = Buffer.concat([Buffer.alloc(MAX_LINE_BYTES + 1, 0x61), Buffer.from([0x0a])]);
    const r = sp.push(big);
    expect(r.oversized).toBe(true);
  });
});

describe('schema rejection of malformed messages', () => {
  it('schema-rejects unknown type', () => {
    const dec = decodeMessage(JSON.stringify({ type: 'WHO_KNOWS' }));
    expect(dec.ok).toBe(false);
    if (!dec.ok) {
      expect(dec.kind).toBe('schema');
    }
  });

  it('schema-rejects HELLO with non-numeric pid', () => {
    const dec = decodeMessage(JSON.stringify({ type: 'HELLO', v: 1, pid: 'oops', uid: 0 }));
    expect(dec.ok).toBe(false);
    if (!dec.ok) {
      expect(dec.kind).toBe('schema');
    }
  });

  it('schema-rejects RESPONSE error with bad kind', () => {
    const dec = decodeMessage(
      JSON.stringify({
        type: 'RESPONSE',
        id: 'r',
        ok: false,
        error: { kind: 'fictional_kind' },
      }),
    );
    expect(dec.ok).toBe(false);
    if (!dec.ok) {
      expect(dec.kind).toBe('schema');
    }
  });
});

// Type-check the AnyMessage union has the expected discriminants.
const _typeCheck: AnyMessage['type'] | null = null;
void _typeCheck;
void HelloSchema;
void HelloOkSchema;
void AuthSchema;
void AuthOkSchema;
void AuthDenySchema;
void RequestSchema;
void ResponseSchema;
