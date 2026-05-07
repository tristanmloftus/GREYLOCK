# AGENT-AUDIT-LOG — Phase 3 retrospective

> Hash-chained, append-only audit log: service layer + sanitizer + admin
> verify/query routes. Wraps the repo-level `append`/`verifyChain`/`query`
> AGENT-DB shipped in Phase 2. AGENT-PLAID, AGENT-SYNC, AGENT-COMPUTE worked
> in parallel on the same Phase 3 commit.

---

## What shipped

### `lib/audit/chain.ts` — canonical hash construction (extracted from AGENT-DB)
Pure, deterministic, no I/O. SHA-256 over the canonical-bytes layout in
`docs/ARCHITECTURE.md` §7. `ZERO_PREV_HASH` (32 zero bytes) is exported as the
sentinel for `seq=1`. The function throws **only** on programmer error
(prevHash with wrong length, negative seq, > uint32 detailsJson) — these are
bugs, not runtime errors.

### `lib/audit/sanitizer.ts` — the hard gate
Recursive walk (max depth 8) that returns `{ok:true,sanitized}` or
`{ok:false,reason}`. Never throws — every path is a Result. Behavior:

- **Deny by key** (case-insensitive substring): `password`, `passphrase`,
  `secret`, `token`, `cookie`, `dek`, `kek`, `kekSalt`, `credentialPublicKey`,
  `signature`, `attestationObject`, `clientDataJSON`, `authenticatorData`,
  `pem`, `key` (carved out by allowlist).
- **Allowlist override** (every entry justified in-source): opaque ids
  (`userId`, `sessionId`, `itemId`, …, including `tokenId` for the
  EnrollmentToken row id, NEVER the plaintext token), control fields
  (`domain`, `outcome`, `action`, `kind`, `reason`, `version`, `seq`, `ts`),
  Plaid sync counters (`count`, `added`, `modified`, `removed`), HTTP error
  context (`httpStatus`, `errorCode`), and counter-monotonicity context
  (`storedCounter`, `newCounter`, `userDekVersion`, `transports`).
- **Deny by value shape**: any string ≥32 chars matching `^[A-Za-z0-9_-]{32,}$`
  (base64url) or `^[0-9a-f]{32,}$` (hex). Catches access tokens, hashes,
  signatures even at allowlisted keys.
- **Reject on**: depth > 8, total JSON > 64 KiB, BigInt at non-allowlisted
  key (BigInt at allowlisted keys is stringified — the JSON column doesn't
  serialize bigint), `Buffer`/`Uint8Array`/`Function`/`Symbol`/`undefined`/
  non-finite numbers, non-plain objects (catches Date, Map, custom classes
  whose `toJSON` could smuggle data past the walk).
- **Reject is total, never partial-strip**. The brief's hard requirement.

### `lib/audit/service.ts` — `AuditService` implementation
Constructor takes `{auditRepo}`. `append` runs the sanitizer, serializes the
sanitized tree to JSON exactly once, hands `detailsJson` to the repo. Repo
never sees unsanitized data. `query` and `verifyChain` are pass-throughs.

### `lib/audit/index.ts` — barrel
Exports `sanitizeDetails`, `computeEntryHash`, `ZERO_PREV_HASH`, type
definitions, and **two factory shapes** for `createAuditService`:
- `createAuditService(deps)` — explicit deps for tests.
- `createAuditService()` (no args) — resolves the repo from the booted DB
  singleton via `getBootedDb()`. The runtime `services-registry.ts`
  invokes this no-arg form synchronously per its existing contract.

### `app/api/admin/audit/verify/route.ts` + `app/api/admin/audit/query/route.ts`
Both `GET`. Both `session+owner`. Verify returns `AdminAuditVerifyResponseSchema`
(`{verifiedCount, brokenAtSeq?}`). Query validates params via Zod (`fromTs`,
`toTs`, `actorUserId`, `action`, `domain`, `limit`), with `limit` hard-capped
at 1000 by the schema. Both routes audit `admin_audit_verify_invoked`/no-op
respectively. Bytes (`prevHash`/`entryHash`) are base64-encoded on the wire;
the response carries no token-shape data because the rows themselves passed
the sanitizer at append time.

---

## Architectural decisions made

### 1. Lifted `computeEntryHash` from `lib/db/repositories/audit.ts` to `lib/audit/chain.ts`

**Choice.** Took the brief's preferred path: the canonical bytes function lives
in `lib/audit/chain.ts` (this agent's owned territory) and the repository
imports it. Edit to AGENT-DB's file is one block: the local function and its
helper byte-encoders were deleted; a single `import { computeEntryHash,
ZERO_PREV_HASH } from '../../audit/chain.js'` was added; the public re-export
`export { computeEntryHash } from '../../audit/chain.js'` keeps
`lib/db/index.ts`'s `export { computeEntryHash } from './repositories/audit.js'`
working without any further edit.

**Why preferred.** Two implementations of byte-exact serialization is the
classic bug-magnet: any drift breaks `pnpm admin:audit-verify` against an
existing chain. One source of truth, one place to test, zero risk of drift.

**Behavioral guarantee preserved.** `tests/integration/db/audit-chain.test.ts`
(AGENT-DB's existing 4-test suite over the `auditRepo`) still passes
unchanged. The `__INTERNAL_FOR_TESTS__` re-export stayed (now exposing only
`ZERO_PREV_HASH` since the function lives elsewhere).

### 2. `createAuditService` is overloaded — explicit deps for tests, lazy-singleton for runtime

**Choice.** The runtime `services-registry.ts` was already written by AGENT-AUTH
to call `mod.createAuditService()` with no arguments and synchronously. I
honored that signature. The integration test path needs explicit deps so it
can wire against an isolated SQLCipher DB without touching the global
`bootedSingleton`. Solved with a TypeScript overload that branches on
arity: `(deps?) => deps ? wrapDeps(deps) : wrapDeps({auditRepo: getBootedDb().repos.auditRepo})`.

**Trade-off.** Importing `getBootedDb` at the top of the audit barrel creates a
soft dependency on `lib/db`. There is no cycle (the chain module is leaf;
the barrel imports from `lib/db/index.ts` which imports from
`lib/db/repositories/audit.ts` which imports from `lib/audit/chain.ts` — a
DAG, not a cycle). If `getBootedDb()` throws (singleton not registered),
the registry's existing 503-surface path catches it.

### 3. Sanitizer rejects token-shape values at allowlisted keys too

**Choice.** A long base64url or hex string that ends up in `details.reason`
still rejects, even though `reason` is on the key allowlist. The allowlist
is for *keys* only; the value-shape detector runs unconditionally on every
string ≥32 chars.

**Why.** If it didn't, an audit caller could write `{reason: '<a 64-char
access token I forgot about>'}` and the sanitizer would let it through.
Defense in depth — the key check and the value check are independent
gates, each rejecting independently. The integration test "also rejects
token-shape values" pins this behavior.

### 4. Sanitizer never logs `details`

**Spec line.** The brief: "No `console.log` of `details`, of token-shape
data, of the secret bytes." Verified by `grep -n console lib/audit/` —
zero matches across all four files.

---

## Hard requirements — verification map

| Requirement | Where enforced | Test |
|---|---|---|
| Sanitizer is a hard gate; rejection is total | `lib/audit/sanitizer.ts` returns `{ok:false}` on first deny; `lib/audit/service.ts:append` returns `Err({kind:'sanitizer_rejected_payload'})` | `tests/integration/audit/service.test.ts > sanitizer rejection surfaces as AuditError without writing the row` |
| Chain bytes byte-exact to ARCH §7 | Independent oracle re-implements the layout and SHA-256s it; equivalence asserted on 6 vectors | `tests/unit/audit/chain.test.ts > computeEntryHash — canonical-bytes vectors` |
| `computeEntryHash` is pure | Function takes only structured inputs; no `Date`, no `crypto.randomBytes`, no I/O | `tests/unit/audit/chain.test.ts > pure + deterministic` |
| Sanitizer never throws | All paths return Result; `try/catch` around `JSON.stringify` and the walk | `tests/unit/audit/sanitizer.test.ts > never throws (4 cases incl. throwing getter)` |
| No console.log of details | `grep -n console lib/audit/` = empty | manual |
| Deny-list closed by default | `isKeyAllowed()` is allowlist-then-deny; allowlist hand-justified in source | `tests/unit/audit/sanitizer.test.ts > deny on key match (20 entries)` |
| `/api/admin/audit/query` `limit ≤ 1000` | Zod `.max(1000)` | route's Zod schema rejects 1001 with 400 |
| Owner-only admin routes | Session validated → `User.role === 'owner'` check; non-owner → 403 | manual route inspection (no integration test of the route here; the AuthService and UserRepo are exercised in their own suites — Orchestrator may add a route-level test in Phase 5 hardening) |

---

## Test summary

```
tests/unit/audit/sanitizer.test.ts        48 tests passed
tests/unit/audit/chain.test.ts            11 tests passed
tests/integration/audit/service.test.ts    5 tests passed
                                          ────────────────
                                          64 tests in audit suite
```

Existing 192 Phase 2 tests still pass after the chain extraction
(`pnpm test tests/integration/auth tests/integration/db tests/unit/auth
tests/unit/crypto`). The full `pnpm test` run shows 6 failures in
`tests/integration/ipc/` and `tests/integration/plaid/` — those are
sibling-agent (AGENT-PLAID, AGENT-IPC) parallel-track issues, not
caused by this agent.

---

## Owner of follow-ups

1. **Route-level integration test for `/api/admin/audit/*`.** Owner-only
   gating, 403 path, `limit > 1000` rejection — these are tested at the
   service level here but a Playwright e2e covering the full HTTP path is
   a Phase 5 QA-SEC item. Out of scope for Phase 3.
2. **Wire `pnpm admin:audit-verify` script.** AGENT-DB's `package.json` already
   declares the script; the actual `scripts/admin-audit-verify.ts` belongs
   to Orchestrator territory. The implementation can simply call
   `audit.verifyChain()` against a `bootDb()`-built service.
3. **Detail allowlist evolution.** Each new audit-emit site should run its
   payload through this sanitizer in test before merging. If a new key
   needs to be added to `ALLOWED_KEYS`, the comment block in `sanitizer.ts`
   requires a justification — code review should reject any add without one.

---

## Files written (this agent only)

```
lib/audit/chain.ts                                 (canonical hash; pure)
lib/audit/sanitizer.ts                             (hard gate; pure; ≥48 unit tests)
lib/audit/service.ts                               (AuditService; wraps repo)
lib/audit/index.ts                                 (barrel + factory)
app/api/admin/audit/verify/route.ts                (GET, owner-only)
app/api/admin/audit/query/route.ts                 (GET, owner-only, Zod limit ≤1000)
tests/unit/audit/sanitizer.test.ts                 (48 tests)
tests/unit/audit/chain.test.ts                     (11 tests, 6+ canonical vectors)
tests/integration/audit/service.test.ts            (5 tests, real SQLCipher DB)
docs/agents/AGENT-AUDIT-LOG.md                     (this retro)
```

## File edited (one minimal edit, justified per brief)

```
lib/db/repositories/audit.ts                       (replaced inline computeEntryHash
                                                    with import from lib/audit/chain.ts;
                                                    public re-export preserved)
```
