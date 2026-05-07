// Greylock — audit-log details sanitizer
// =============================================================================
// AGENT-AUDIT-LOG (Phase 3). The hard gate that prevents secrets from
// reaching the audit `details` payload. Any append whose details fail this
// pass surfaces as `AuditError {kind: 'sanitizer_rejected_payload'}` upstream
// — the partial-strip alternative is forbidden because it would silently
// retain whichever fields the deny-list missed.
//
// Threat model reference: docs/THREAT_MODEL.md §1.2.2 (insider tampers PCC),
// docs/ARCHITECTURE.md §7 ("What's NEVER logged"), docs/SPEC.md §7
// (anti-patterns).
//
// Behavior contract:
//   - Pure. No I/O. Never throws — every path returns a Result-shape
//     `{ok: true, sanitized}` or `{ok: false, reason}`.
//   - Recursive walk of the input object tree, capped at depth 8. Anything
//     deeper rejects.
//   - Total serialized JSON size capped at 64 KiB; over → reject. (The
//     bound is a SPEC §11.4 promise; AuditService also enforces it.)
//   - Deny-list closed by default for KEYS. Adding a key to the allowlist
//     requires a comment justifying why it's safe.
//   - Deny-list closed by default for VALUES of token shape (long base64url
//     or hex). Length floor 32 chars — short identifiers slip through, full
//     access tokens / hashes / signatures don't.
//   - BigInt values at allowlisted keys are stringified (BigInt is not
//     JSON-serializable; ARCHITECTURE.md §7 stores `detailsJson` as a TEXT
//     column). BigInt anywhere else rejects.
//   - Unsafe value kinds (Buffer, Uint8Array, Function, Symbol) reject —
//     these have no business in audit metadata.
// =============================================================================

const MAX_DEPTH = 8;
const MAX_TOTAL_BYTES = 64 * 1024;

// -----------------------------------------------------------------------------
// Allowlist — keys we allow even if they substring-match the deny-list.
// Each entry below is justified: NONE of these carry token bytes, key bytes,
// or unmasked PII. Rows that wrap encrypted blobs (e.g. `User.wrappedUserDek`)
// NEVER reach this sanitizer — they live in repository code paths that don't
// touch `details`.
// -----------------------------------------------------------------------------
const ALLOWED_KEYS: ReadonlySet<string> = new Set([
  // Identifiers — opaque CUID-style ids, no secret material.
  'userId',
  'sessionId',
  'itemId',
  'accountId',
  'transactionId',
  'passkeyId',
  'snapshotId',
  // EnrollmentToken row id (the *id*, NEVER the plaintext token).
  'tokenId',
  'subjectId',
  'actorUserId',
  // Domain / control fields.
  'domain',
  'outcome',
  'action',
  'kind',
  'reason',
  'version',
  'seq',
  'ts',
  // Counters and tallies (Plaid sync, etc.).
  'count',
  'added',
  'modified',
  'removed',
  // Plaid / HTTP error context (codes are short literals, not secrets).
  'httpStatus',
  'errorCode',
  // Counter values for replay-defence audit context.
  'storedCounter',
  'newCounter',
  // userDekVersion is a small integer (never key bytes); audit context.
  'userDekVersion',
  // Transports — small string array of 'usb','ble','nfc','internal'.
  'transports',
]);

// -----------------------------------------------------------------------------
// Deny-list (case-insensitive substring match). A *key* containing any of
// these strings rejects unless it's in ALLOWED_KEYS.
// -----------------------------------------------------------------------------
const DENY_KEY_SUBSTRINGS: ReadonlyArray<string> = [
  'password',
  'passphrase',
  'secret',
  'token', // covers access_token, refresh_token, link_token, enrollmentToken, …
  'cookie',
  'dek',
  'kek',
  'keksalt',
  'credentialpublickey',
  'signature',
  'attestationobject',
  'clientdatajson',
  'authenticatordata',
  'pem',
  'key', // intentionally last; carve-out via ALLOWED_KEYS
];

// -----------------------------------------------------------------------------
// Token-shape value patterns. Substring-anchored; we match on the WHOLE
// string — partial-content strings (e.g. error messages mentioning a hash)
// don't trigger here, but a bare hash value does.
// -----------------------------------------------------------------------------
//
// We use `RegExp` literals here. eslint-plugin-security flags
// `detect-unsafe-regex` — these patterns are linear (no nested quantifiers)
// so they're safe; we still annotate explicitly for the human reviewer.
// -----------------------------------------------------------------------------
const BASE64URL_LIKE = /^[A-Za-z0-9_-]{32,}$/u;
const HEX_LIKE = /^[0-9a-f]{32,}$/u;

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

export type SanitizerResult =
  | { readonly ok: true; readonly sanitized: Record<string, unknown> }
  | { readonly ok: false; readonly reason: string };

/**
 * Recursively walk `details` and return either the cleaned tree or a
 * rejection reason. Never throws.
 *
 * The rejection reason is intentionally generic ("deny key 'password'",
 * "token-shape value", "depth limit") so the caller can pass it to the
 * AuditService without leaking actual payload contents.
 */
export function sanitizeDetails(
  details: Record<string, unknown>,
): SanitizerResult {
  let walked: Record<string, unknown>;
  try {
    const w = walkObject(details, 0, 'details');
    if (!w.ok) {
      return { ok: false, reason: w.reason };
    }
    walked = w.value;
  } catch (cause: unknown) {
    // Defensive: walkObject is engineered not to throw, but if a getter on
    // the input object throws (rare), we still don't propagate.
    const msg = cause instanceof Error ? cause.message : 'unknown';
    return { ok: false, reason: `walk threw: ${msg}` };
  }

  // Total-size cap — measured on the canonical JSON we'll actually persist.
  let serialized: string;
  try {
    serialized = JSON.stringify(walked);
  } catch (cause: unknown) {
    const msg = cause instanceof Error ? cause.message : 'unknown';
    return { ok: false, reason: `unable to serialize: ${msg}` };
  }
  if (Buffer.byteLength(serialized, 'utf8') > MAX_TOTAL_BYTES) {
    return { ok: false, reason: 'payload exceeds 64 KiB' };
  }

  return { ok: true, sanitized: walked };
}

// -----------------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------------

type WalkResult<T> =
  | { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly reason: string };

function walkObject(
  input: Record<string, unknown>,
  depth: number,
  path: string,
): WalkResult<Record<string, unknown>> {
  if (depth > MAX_DEPTH) {
    return { ok: false, reason: `depth limit at ${path}` };
  }
  const out: Record<string, unknown> = {};
  for (const key of Object.keys(input)) {
    if (!isKeyAllowed(key)) {
      return { ok: false, reason: `deny key '${key}' at ${path}` };
    }
    // Read + write use a key drawn from `Object.keys(input)` — a real
    // own-property string, not external user input. Disabling the
    // object-injection rule for these two lines is precise and justified.
    // eslint-disable-next-line security/detect-object-injection
    const value = input[key];
    const v = walkValue(value, depth + 1, `${path}.${key}`, key);
    if (!v.ok) {
      return v;
    }
    // eslint-disable-next-line security/detect-object-injection
    out[key] = v.value;
  }
  return { ok: true, value: out };
}

function walkArray(
  input: ReadonlyArray<unknown>,
  depth: number,
  path: string,
): WalkResult<ReadonlyArray<unknown>> {
  if (depth > MAX_DEPTH) {
    return { ok: false, reason: `depth limit at ${path}` };
  }
  const out: unknown[] = [];
  for (let i = 0; i < input.length; i++) {
    // Numeric index from a controlled loop bound; not user-influenced.
    // eslint-disable-next-line security/detect-object-injection
    const value = input[i];
    const v = walkValue(value, depth + 1, `${path}[${String(i)}]`, null);
    if (!v.ok) {
      return v;
    }
    out.push(v.value);
  }
  return { ok: true, value: out };
}

/**
 * Walk a single value. `parentKey` is non-null when this value sits directly
 * under an allowlisted object key — that key's allowlist status authorizes
 * BigInt stringification (without it, BigInt rejects everywhere).
 */
function walkValue(
  value: unknown,
  depth: number,
  path: string,
  parentKey: string | null,
): WalkResult<unknown> {
  if (value === null) {
    return { ok: true, value: null };
  }
  if (typeof value === 'string') {
    if (looksLikeToken(value)) {
      return { ok: false, reason: `token-shape value at ${path}` };
    }
    return { ok: true, value };
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) {
      return { ok: false, reason: `non-finite number at ${path}` };
    }
    return { ok: true, value };
  }
  if (typeof value === 'boolean') {
    return { ok: true, value };
  }
  if (typeof value === 'bigint') {
    if (parentKey !== null && ALLOWED_KEYS.has(parentKey)) {
      return { ok: true, value: value.toString() };
    }
    return { ok: false, reason: `bigint at non-allowlisted key (${path})` };
  }
  if (typeof value === 'function' || typeof value === 'symbol' || typeof value === 'undefined') {
    return { ok: false, reason: `unsupported value kind at ${path}` };
  }
  // Disallow Buffer / Uint8Array / typed-array. JSON would lose them anyway,
  // and they're often token-shape bytes a caller forgot to base64-encode.
  if (value instanceof Uint8Array || ArrayBuffer.isView(value)) {
    return { ok: false, reason: `binary value at ${path}` };
  }
  if (Array.isArray(value)) {
    return walkArray(value, depth, path);
  }
  if (typeof value === 'object') {
    // Reject objects with a non-default toJSON — they could smuggle data
    // past our walk by serializing differently than they look. We allow
    // plain objects only.
    if (!isPlainObject(value)) {
      return { ok: false, reason: `non-plain object at ${path}` };
    }
    return walkObject(value as Record<string, unknown>, depth, path);
  }
  return { ok: false, reason: `unknown value kind at ${path}` };
}

function isKeyAllowed(key: string): boolean {
  if (ALLOWED_KEYS.has(key)) {
    return true;
  }
  const lower = key.toLowerCase();
  for (const sub of DENY_KEY_SUBSTRINGS) {
    if (lower.includes(sub)) {
      return false;
    }
  }
  return true;
}

function looksLikeToken(s: string): boolean {
  if (s.length < 32) {
    return false;
  }
  // Hex check first — `[0-9a-f]+` is a strict subset of base64url, so a long
  // hex string would also match BASE64URL_LIKE; we still return true either
  // way, but checking hex first lines up the rejection reason with the
  // tighter pattern.
  if (HEX_LIKE.test(s)) {
    return true;
  }
  if (BASE64URL_LIKE.test(s)) {
    return true;
  }
  return false;
}

function isPlainObject(v: object): boolean {
  const proto = Object.getPrototypeOf(v);
  return proto === Object.prototype || proto === null;
}
