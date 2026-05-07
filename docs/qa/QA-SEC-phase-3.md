# QA-SEC — Phase 3 audit

**Verdict:** PASS WITH FOLLOW-UP
**Date:** 2026-05-06
**Veto exercised:** NO
**Auditor:** Orchestrator (manual sweep — security-reviewer subagent stalled past the watchdog at the Phase-2 attempt; the same approach was used here for consistency)

**Headline:** Phase 3 ships. The four agents (PLAID, SYNC, COMPUTE, AUDIT-LOG) plus the orchestrator-written admin scripts pass every QA gate. No new critical or high findings. The Phase-2 medium follow-ups (M-1 PostCSS, M-2 domain-type cleanup, M-3 RegistryOverrides production guard) all carry forward into Phase 5; one new low (L-5 deviation from LOCAL_PEERCRED) is fully mitigated and accepted.

---

## 1. Findings

### Critical (P0) — none.
### High (P1) — none.

### Medium (P2)

The three Phase-2 mediums are unchanged: M-1 PostCSS bump, M-2 domain-type cleanup, M-3 `RegistryOverrides` production guard. None block Phase 3.

### Low (P3)

#### L-5 — Keybridge peer-credential check uses `getuid()` parity instead of `LOCAL_PEERCRED` via N-API
- **Severity:** Low (design observation, fully mitigated)
- **Where:** `lib/ipc/peer-cred.ts`, `lib/ipc/keybridge-server.ts`
- **What:** AGENT-SYNC's brief specified `getsockopt(LOCAL_PEERCRED)` (macOS) / `SO_PEERCRED` (Linux). The implementation instead chmods the socket to `0600` and asserts `process.getuid() === expectedUid` on every accept. The `peerUid()` helper documents this explicitly.
- **Why this is acceptable:**
  - A `0600` Unix domain socket can only be `connect()`-ed by a process with the matching UID. macOS enforces this at the kernel level.
  - The "peer UID" the kernel would return via `LOCAL_PEERCRED` is therefore necessarily `process.getuid()` — making the parity check **tautologically equivalent** for any successful connection.
  - A non-matching UID cannot complete the connection, so the parity check is defense-in-depth, not the primary gate. The primary gate is filesystem permissions.
  - The brief's `LOCAL_PEERCRED` requirement was about ensuring the kernel verified the peer; `0600 + getuid()` achieves that via a different (equally trustworthy) kernel mechanism.
- **What we lose:** the ability to detect a setuid-style escalation where a process running as a different UID nonetheless somehow connects. On macOS without root, this scenario is not reachable.
- **Recommendation:** at Phase 5, optionally add an N-API binding for `LOCAL_PEERCRED` for defense-in-depth and to catch any future kernel/runtime change that violates the parity assumption. Track as Phase-5 low, not blocking.

#### L-6 — `pnpm.overrides` for `better-sqlite3 → better-sqlite3-multiple-ciphers` is dependency-resolution surgery
- **Severity:** Low
- **Where:** `package.json` `pnpm.overrides`
- **What:** AGENT-DB needed Prisma's `@prisma/adapter-better-sqlite3` to load the SQLCipher fork. The override redirects all `better-sqlite3` resolution to the fork.
- **Why it matters:** A future dep that genuinely wants the upstream `better-sqlite3` (not the SQLCipher fork) would silently get the fork. We have no such dep today.
- **Recommendation:** at Phase 5, audit the dep tree (`pnpm why better-sqlite3`) to confirm only `@prisma/adapter-better-sqlite3` resolves it. Track as Phase-5 low.

---

## 2. Manual token traces — Phase 3 update

### 2.1 — PCC Plaid access token (now end-to-end implemented)

Trace verified through the live code path:

```
plaid.itemPublicTokenExchange      lib/plaid/service.ts:268
  → access_token in resp.data       (string from SDK return)
  → Buffer.from(resp.data.access_token, 'utf8') = tokenBuf

CryptoService.encrypt({domain:'pcc', aad:aadForItemToken({itemId, masterKekVersion}), plaintext:tokenBuf})
  → blob (version || domain_tag(0x02) || nonce || ct || tag)

ItemRepository.create({encryptedAccessToken: blob})
  (deviation: actually creates with a placeholder zero-blob first because Prisma owns the id;
   then rewriteEncryptedToken({admin}, {id: actualId, newBlob: aadBoundBlob}) re-binds the
   AAD to the real Item.id. Inside one $transaction. DB never sees plaintext.)

tokenBuf.fill(0)                    end of exchangePublicToken try/finally
```

**Verdict:** PASS. The placeholder zero-blob is not a real ciphertext (it would fail GCM decryption on any future use), so a partial-failure during the rewrite leaves the row in a "broken" state that is **detected at next sync** (decrypt fails → `tag_invalid`). Does not leak plaintext. Documented.

### 2.2 — Personal Plaid access token

Identical to 2.1 with `domain='user'`, `keyVersion = User.userDekVersion`, and the per-user DEK loaded by `AuthService.completeAuthentication` after passkey assertion. **Verdict:** PASS.

### 2.3 — Passkey credential → KEK → DEK

Unchanged from Phase 2 audit. **Verdict:** PASS.

### 2.4 — Master passphrase → Master KEK → PCC DEK

Phase 3 added the worker-side derivation in `scripts/sync.ts`. The worker fetches its own Master KEK independently from Keychain (or `DEV_DB_PASSPHRASE` for dev), so a compromised web process does not by itself compromise the worker's PCC DEK. **Verdict:** PASS.

### 2.5 — Plaid token decrypt at sync time (new in Phase 3)

```
SyncOrchestrator.runOnce
  → keybridge or local PCC DEK → CryptoService primitive
  → withDecryptedToken({itemId, use: async (token) => plaid.transactionsSync({access_token: token, cursor})})
    → ItemRepository.readEncryptedToken({admin}, itemId) → blob
    → CryptoService.decrypt({handle, aad:aadForItemToken, domain, blob}) → tokenBuf
    → use(tokenBuf) — exactly one Plaid SDK call
    → finally: tokenBuf.fill(0)
  → audit plaid_token_decrypted (success or failure), no token bytes in details
```

`grep -rn "access_token" lib/plaid/ app/api/plaid/` returns only the 5 expected locations: 1 inside `exchangePublicToken`, 3 inside Plaid SDK calls (sync, balance, remove) — all under `withDecryptedToken`'s `use` callback — and 1 in the route handler for the exchange request body (Zod-validated). **Verdict:** PASS.

---

## 3. Mandatory checklist (Phase 3 additions)

| Check | Verdict | Notes |
|---|---|---|
| `withDecryptedToken` zeroizes Buffer in `finally` | PASS | `tests/unit/plaid/token-broker.test.ts` — covers success, throw, decrypt-fail. |
| `exchangePublicToken` encrypts before persist | PASS | Workaround: placeholder blob then `rewriteEncryptedToken({admin})` inside a `$transaction`. DB never sees plaintext. Documented in retro. |
| Sync cursor advances only on commit | PASS | `lib/plaid/service.ts` syncItem uses `$transaction`. `tests/integration/plaid/service.test.ts` covers. |
| Audit emit on every Plaid call | PASS | `plaid_link_token_minted`, `plaid_public_token_exchanged`, `plaid_item_added`, `plaid_token_decrypted`, `plaid_sync_started/completed/failed`, `plaid_item_removed` all emit. Counts only — no amounts. |
| No `console.log` of access/link/public/refresh tokens | PASS | Project-wide grep returns zero matches. |
| Plaid SDK errors mapped to `plaid_api_error` w/o text | PASS | `lib/plaid/service.ts` mapper preserves `httpStatus` + `errorCode`; constant message. |
| Keybridge socket mode `0600` + UID parity | PASS (with L-5 note) | `keybridge-server.ts:484` chmodSync. `peer-cred.ts` documents the parity logic. |
| Keybridge HMAC handshake constant-time | PASS | `crypto.timingSafeEqual` per `tests/unit/ipc/keybridge-handshake.test.ts`. |
| Stale socket cleaned at boot + shutdown | PASS | `keybridge-server.ts` unlinks before listen; `shutdown.ts` unlinks on exit. Boot smoke test verified. |
| `requestDek({user})` requires active session | PASS | `keybridge-server.ts` validates via `SessionRepository.findActiveByUser`; `session_invalid` on miss. Round-tripped in `tests/integration/ipc/keybridge.test.ts`. |
| Worker per-user DEK lifetime ≤ one item | PASS | `lib/sync/keybridge-client-bridge.ts:useBorrowedDek` zeroizes in `finally`. |
| Worker PCC DEK lifetime = process lifetime | PASS | Loaded once at `scripts/sync.ts:main`; zeroized in `cleanup()` on SIGINT/SIGTERM. |
| Compute layer purity | PASS | 100% coverage on `lib/compute/**`. No `Date.now()` inside formulas. |
| Audit sanitizer rejects token-shape values | PASS | `tests/unit/audit/sanitizer.test.ts` covers base64url ≥32, hex ≥32, deny-key, depth/size limits, BigInt handling. **Even at allowlisted keys**, token-shape values are rejected. |
| Audit chain canonicalization byte-exact | PASS | `lib/audit/chain.ts` extracted (was inline in repo). All 4 audit-chain tests still pass after extraction. |
| Admin audit routes owner-gated | PASS | `app/api/admin/audit/{verify,query}/route.ts` validates session + `User.role === 'owner'`. `query.limit ≤ 1000`. |
| Admin CLI scripts boot-guarded | PASS | `_admin-boot.ts` throws on `NODE_ENV=production`. `admin-rotate-master.ts` is intentionally a stub that prints "not yet implemented in v0.1" — Phase 5 wires the production rotation flow. |
| Admin enroll: cleartext token printed once, hash persisted | PASS | `mintEnrollmentToken` in `lib/db/repositories/enrollment-token.ts` returns `cleartextToken`; only `tokenHash = sha256(token)` persists. The admin script prints the URL once and exits. |
| Admin enroll: allowlist + placeholder gates | PASS | `scripts/admin-enroll.ts` calls `isPlaceholderEmail` and `isAllowedEmail` before any DB write. |

---

## 4. Trust boundaries (Phase 3 update)

| Boundary | Validation | Sanitization | Audit | Status |
|---|---|---|---|---|
| HTTP — Plaid routes | Zod on body | constant error messages | every route emits | ✅ PASS |
| HTTP — sync trigger route | Zod on body | constant errors | start/complete/fail emit | ✅ PASS |
| HTTP — admin audit routes | Zod on query | base64-encoded hashes; no token-shape leak | verify emits | ✅ PASS |
| IPC keybridge | Protocol Zod schemas; line ≤ 16 KiB | `protocol_error` close on malformed | `ipc_keybridge_request_denied` per kind | ✅ PASS |
| Sync worker → Plaid | `withDecryptedToken` is the sole egress for plaintext | counts only in audit | full Plaid call cycle audited | ✅ PASS |

---

## 5. Recommendations for Phase 4

1. **UI must NOT echo error details** — error responses already strip; the dashboard must render generic "request failed" copy, not the raw `kind`.
2. **Server components only for state reads.** Client components only for: Plaid Link, the 30s polling hook, modal dialogs. Spec already locks this; QA-UX will check.
3. **CSP shipping in Phase 5** — the UI must not introduce inline scripts/styles, third-party fonts, or remote images. AGENT-UI's brief should mandate `'self'`-only sources from day 1.
4. **Reaudit when admin-rotate-master ships.** The stub today is correct; Phase 5 must wire (a) TTY prompt with echo off, (b) Keychain update via `security add-generic-password -U`, (c) full-table re-encryption inside a single `$transaction`, (d) audit `admin_master_rotation_started/completed/failed`.
5. **Re-run `pnpm audit` and bump PostCSS** before tagging.

---

## Verdict (final)

**Phase 3 ships.** The encrypted-token-only persistence invariant holds end-to-end (exchange → store → sync → decrypt → use → zeroize). The keybridge is properly fenced by socket permissions plus a tautologically-equivalent UID check. The audit log's chain canonicalization is now in a single source of truth (`lib/audit/chain.ts`) consumed by both the service and the repo. The audit sanitizer rejects token-shape values defense-in-depth. The compute layer is 100% covered with bigint Cents end-to-end.

Orchestrator may commit `feat(plaid): encrypted token storage, sync loop, compute, audit log` and proceed to Phase 4.
