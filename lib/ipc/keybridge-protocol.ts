// Greylock — IPC keybridge wire format
// =============================================================================
// AGENT-SYNC (Phase 3). Pure types + Zod schemas describing every line ever
// written to the keybridge socket. No I/O. No crypto. The server and client
// both encode/decode through this module so the wire format is exercised by
// the unit tests in tests/unit/ipc/keybridge-protocol.test.ts.
//
// Wire format (newline-delimited JSON, lines <= 16 KiB):
//   line 1  HELLO       { v: 1, pid, uid }
//   line 2  HELLO_OK    { v: 1, serverNonce: <base64-32B> }
//   line 3  AUTH        { clientNonce: <base64-32B>, hmac: <base64-32B> }
//   line 4  AUTH_OK     { hmac: <base64-32B> }
//                or AUTH_DENY { reason }
//   line N  REQUEST     { id, method: 'requestDek' | 'releaseDek' | 'ping', params }
//                       RESPONSE { id, ok, result?, error? }
//
// Binary fields (nonces, HMACs, DEK bytes) are base64-encoded so the JSON
// itself stays text-only.
//
// MAX_LINE_BYTES is the canonical "drop the connection" guard. A peer that
// sends more than 16 KiB before a newline is malicious or broken; we reject
// the line and audit `protocol_error`.
// =============================================================================

import { z } from 'zod';

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

export const PROTOCOL_VERSION = 1;
/** Hard cap per protocol line. Lines longer than this are rejected. */
export const MAX_LINE_BYTES = 16 * 1024;
/** Length of nonces (handshake server/client) in bytes. */
export const NONCE_BYTES = 32;
/** Length of HMAC-SHA-256 output in bytes. */
export const HMAC_BYTES = 32;
/** Length of a DEK in bytes (must match `lib/crypto/kdf.ts` KEY_LENGTH_BYTES). */
export const DEK_BYTES = 32;

// -----------------------------------------------------------------------------
// Base64 helper schema — non-empty, base64 (with padding)
// -----------------------------------------------------------------------------

const Base64StringSchema = z
  .string()
  .min(1)
  .regex(/^[A-Za-z0-9+/]+={0,2}$/u, 'must be base64');

const Base64Of = (byteLength: number): z.ZodEffects<typeof Base64StringSchema, string, string> =>
  Base64StringSchema.refine(
    (s) => {
      try {
        return Buffer.from(s, 'base64').byteLength === byteLength;
      } catch {
        return false;
      }
    },
    { message: `must decode to exactly ${String(byteLength)} bytes` },
  );

// -----------------------------------------------------------------------------
// Handshake messages
// -----------------------------------------------------------------------------

export const HelloSchema = z.object({
  type: z.literal('HELLO'),
  v: z.literal(PROTOCOL_VERSION),
  pid: z.number().int().nonnegative(),
  uid: z.number().int().nonnegative(),
});
export type Hello = z.infer<typeof HelloSchema>;

export const HelloOkSchema = z.object({
  type: z.literal('HELLO_OK'),
  v: z.literal(PROTOCOL_VERSION),
  serverNonce: Base64Of(NONCE_BYTES),
});
export type HelloOk = z.infer<typeof HelloOkSchema>;

export const AuthSchema = z.object({
  type: z.literal('AUTH'),
  clientNonce: Base64Of(NONCE_BYTES),
  hmac: Base64Of(HMAC_BYTES),
});
export type Auth = z.infer<typeof AuthSchema>;

export const AuthOkSchema = z.object({
  type: z.literal('AUTH_OK'),
  hmac: Base64Of(HMAC_BYTES),
});
export type AuthOk = z.infer<typeof AuthOkSchema>;

export const AuthDenySchema = z.object({
  type: z.literal('AUTH_DENY'),
  reason: z.string().min(1).max(64),
});
export type AuthDeny = z.infer<typeof AuthDenySchema>;

// -----------------------------------------------------------------------------
// Request / response messages
// -----------------------------------------------------------------------------

export const RequestDekParamsSchema = z.union([
  z.object({ kind: z.literal('pcc') }),
  z.object({
    userId: z.string().min(1).max(64),
    sessionId: z.string().min(1).max(64),
  }),
]);
export type RequestDekParams = z.infer<typeof RequestDekParamsSchema>;

export const ReleaseDekParamsSchema = z.object({
  handle: z.union([
    z.object({
      kind: z.literal('pcc'),
      version: z.number().int().positive(),
    }),
    z.object({
      kind: z.literal('user'),
      userId: z.string().min(1).max(64),
      version: z.number().int().positive(),
    }),
  ]),
});
export type ReleaseDekParams = z.infer<typeof ReleaseDekParamsSchema>;

export const PingParamsSchema = z.object({}).strict();
export type PingParams = z.infer<typeof PingParamsSchema>;

export const RequestSchema = z.object({
  type: z.literal('REQUEST'),
  id: z.string().min(1).max(64),
  method: z.enum(['requestDek', 'releaseDek', 'ping']),
  params: z.unknown(),
});
export type Request = z.infer<typeof RequestSchema>;

export const ResponseHandleSchema = z.union([
  z.object({
    kind: z.literal('pcc'),
    version: z.number().int().positive(),
  }),
  z.object({
    kind: z.literal('user'),
    userId: z.string().min(1).max(64),
    version: z.number().int().positive(),
  }),
]);
export type ResponseHandle = z.infer<typeof ResponseHandleSchema>;

export const ResponseDekResultSchema = z.object({
  handle: ResponseHandleSchema,
  dekB64: Base64Of(DEK_BYTES),
});
export type ResponseDekResult = z.infer<typeof ResponseDekResultSchema>;

export const ResponseErrorKindSchema = z.enum([
  'socket_unavailable',
  'peer_credential_mismatch',
  'auth_failed',
  'session_invalid',
  'dek_unavailable',
  'protocol_error',
  'timeout',
]);
export type ResponseErrorKind = z.infer<typeof ResponseErrorKindSchema>;

export const ResponseSchema = z.object({
  type: z.literal('RESPONSE'),
  id: z.string().min(1).max(64),
  ok: z.boolean(),
  result: z.unknown().optional(),
  error: z.object({ kind: ResponseErrorKindSchema }).optional(),
});
export type Response = z.infer<typeof ResponseSchema>;

// -----------------------------------------------------------------------------
// Encoding helpers
// -----------------------------------------------------------------------------

/**
 * Serialize a message object to a single newline-terminated UTF-8 line.
 * Throws if the encoded bytes exceed MAX_LINE_BYTES.
 */
export function encodeLine(message: object): Buffer {
  const json = JSON.stringify(message);
  const buf = Buffer.from(`${json}\n`, 'utf8');
  if (buf.byteLength > MAX_LINE_BYTES) {
    throw new Error('keybridge-protocol: encoded line exceeds MAX_LINE_BYTES');
  }
  return buf;
}

export type DecodeResult<T> =
  | { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly kind: 'oversized' | 'malformed_json' | 'schema' };

/** Parse a single line of JSON. Empty lines / whitespace lines are rejected. */
export function decodeJson(line: Buffer | string): DecodeResult<unknown> {
  const buf = typeof line === 'string' ? Buffer.from(line, 'utf8') : line;
  if (buf.byteLength > MAX_LINE_BYTES) {
    return { ok: false, kind: 'oversized' };
  }
  const text = buf.toString('utf8').trim();
  if (text.length === 0) {
    return { ok: false, kind: 'malformed_json' };
  }
  try {
    return { ok: true, value: JSON.parse(text) as unknown };
  } catch {
    return { ok: false, kind: 'malformed_json' };
  }
}

/**
 * Parse + Zod-validate a line against the union of HELLO/HELLO_OK/AUTH/...
 * messages. Returns a tagged result so callers don't have to reach into Zod.
 */
export type AnyMessage =
  | Hello
  | HelloOk
  | Auth
  | AuthOk
  | AuthDeny
  | Request
  | Response;

const AnyMessageSchema = z.discriminatedUnion('type', [
  HelloSchema,
  HelloOkSchema,
  AuthSchema,
  AuthOkSchema,
  AuthDenySchema,
  RequestSchema,
  ResponseSchema,
]);

export function decodeMessage(line: Buffer | string): DecodeResult<AnyMessage> {
  const json = decodeJson(line);
  if (!json.ok) {
    return json;
  }
  const parsed = AnyMessageSchema.safeParse(json.value);
  if (!parsed.success) {
    return { ok: false, kind: 'schema' };
  }
  return { ok: true, value: parsed.data };
}

// -----------------------------------------------------------------------------
// Newline framing — buffer split on '\n', emit complete lines.
// -----------------------------------------------------------------------------

export interface LineSplitter {
  /** Append bytes to the internal buffer; returns any complete lines made
   *  available by the append. Returns `oversized: true` on the first overflow
   *  and discards the rest; the caller should close the connection. */
  push(chunk: Buffer): { readonly lines: ReadonlyArray<Buffer>; readonly oversized: boolean };
}

export function createLineSplitter(): LineSplitter {
  let pending: Buffer = Buffer.alloc(0);
  let oversizedFlag = false;
  return {
    push(chunk: Buffer): { readonly lines: ReadonlyArray<Buffer>; readonly oversized: boolean } {
      if (oversizedFlag) {
        return { lines: [], oversized: true };
      }
      pending = Buffer.concat([pending, chunk]);
      const lines: Buffer[] = [];
      // Walk through all newlines.
      let idx = pending.indexOf(0x0a);
      while (idx !== -1) {
        const line = pending.subarray(0, idx);
        pending = pending.subarray(idx + 1);
        if (line.byteLength > MAX_LINE_BYTES) {
          oversizedFlag = true;
          return { lines, oversized: true };
        }
        lines.push(Buffer.from(line));
        idx = pending.indexOf(0x0a);
      }
      // Remaining (non-newline-terminated) bytes — check if they alone exceed
      // the budget so we fail-fast before a malicious peer fills RAM.
      if (pending.byteLength > MAX_LINE_BYTES) {
        oversizedFlag = true;
        return { lines, oversized: true };
      }
      return { lines, oversized: false };
    },
  };
}
