# Greylock — API Surface

Phase 1 deliverable. Every HTTP route, the IPC keybridge wire format, and the admin CLI surface. Schemas named here are exported from `lib/types/zod-schemas.ts`. Service interfaces live in `lib/types/services.ts`.

If this doc disagrees with `docs/SPEC.md`, SPEC wins.

---

## Conventions

- All routes are under `https://localhost:3000`.
- All request/response bodies are JSON. `Content-Type: application/json` required on writes.
- Every route validates its request body / query / route params with Zod **before** any side effect.
- Every error response uses `ErrorResponseSchema`:
  ```json
  { "error": { "kind": "<error_kind>", "message": "<human-readable>", "details": { ... } } }
  ```
  `kind` values come from `AuthError | PlaidError | CryptoError | RepoError | AuditError`.
- HTTP status: `400` for validation, `401` for missing/invalid session, `403` for authorized-but-not-allowed (e.g. PCC route called by non-member), `404` for not-found-or-out-of-scope (returned indistinguishably — see threat model), `409` for conflicts, `429` for rate-limit, `500` for unexpected.
- **Auth requirements** (one of):
  - `none` — public, but gated by rate limit.
  - `enrollmentToken` — short-lived one-shot URL token from `pnpm admin:enroll`.
  - `session` — valid iron-session cookie; sliding window touches `lastActivityAt`.
  - `session+pccMember` — `session` plus an active `PccMembership` row.
  - `session+owner` — `session` plus `User.role === 'owner'`.

---

## 1. Health

### `GET /api/healthz`
- **Auth:** `none`. **Rate limit:** 60/min/IP.
- **Response:** `200`
  ```json
  { "ok": true, "checks": { "db": "ok", "crypto": "ok", "keybridge": "ok" } }
  ```
- **Audit:** none.

---

## 2. Auth — passkey enrollment

### `POST /api/auth/registration/begin`
- **Auth:** `enrollmentToken` (one-shot URL).
- **Body:** `RegistrationBeginRequestSchema`
- **Response:** `200` `RegistrationBeginResponseSchema` (challenge + WebAuthn options).
  - Stashes the challenge in an ephemeral iron-session cookie scoped to the registration ceremony.
- **Errors:** `email_not_allowlisted` (400) · `placeholder_email_rejected` (400) · `passkey_already_enrolled` (409) · `rate_limited` (429).
- **Audit:** `passkey_enrollment_rejected` on validation failure; nothing on success (the *complete* call audits).

### `POST /api/auth/registration/complete`
- **Auth:** `enrollmentToken` + ceremony cookie.
- **Body:** `RegistrationCompleteRequestSchema` (`response`, `expectedChallenge`, `deviceLabel`).
- **Side effects:**
  1. `AuthService.completeEnrollment` → verifies attestation, persists `User` + `Passkey`, derives a fresh per-user DEK with `crypto.randomBytes(32)`, wraps under per-user KEK derived from `(credentialId, kekSalt)`, persists `User.wrappedUserDek`.
  2. Invalidates the enrollment-URL token.
- **Response:** `200` `RegistrationCompleteResponseSchema` `{ userId, passkeyId }`.
- **Errors:** `webauthn_verification_failed` (400) · `email_not_allowlisted` (400) · `placeholder_email_rejected` (400) · `passkey_already_enrolled` (409).
- **Audit:** `passkey_enrollment` (success or `failure`).

---

## 3. Auth — passkey authentication

### `POST /api/auth/authentication/begin`
- **Auth:** `none`. **Rate limit:** `AUTH_RATE_LIMIT_ATTEMPTS` (5) per `AUTH_RATE_LIMIT_WINDOW_MINUTES` (15) per `(IP, email)` bucket.
- **Body:** `AuthenticationBeginRequestSchema` `{ email }`.
- **Response:** `200` `AuthenticationBeginResponseSchema` (challenge + `allowCredentials`).
  - Stashes challenge in ceremony cookie.
- **Errors:** `no_passkey_for_email` (404 — returned **indistinguishably** from `email_not_allowlisted` to prevent enumeration) · `rate_limited` (429).
- **Audit:** `passkey_authentication_failure` on validation failure; nothing on success (the *complete* call audits).

### `POST /api/auth/authentication/complete`
- **Auth:** `none` (the assertion *is* the auth). **Rate limit:** same bucket as `begin`.
- **Body:** `AuthenticationCompleteRequestSchema` (`response`, `expectedChallenge`).
- **Side effects, in order:**
  1. `AuthService.completeAuthentication` → verifies via `verifyAuthenticationResponse`, asserts `newCounter > storedCounter` (replay defense), bumps `Passkey.counter`.
  2. Single-session enforcement: revoke any existing active session for this user (`reason='new_login'`).
  3. Create new `Session` (`expiresAt = now + 8h`, `idleTimeoutAt = now + 30m`).
  4. Set iron-session cookie (`SameSite=Strict; Secure; HttpOnly; Path=/`).
  5. Derive per-user KEK from `(credentialId, kekSalt)`, unwrap `User.wrappedUserDek`, hold DEK in `CryptoService` keyed by `userId`.
  6. Audit `session_created`.
- **Response:** `200` `AuthenticationCompleteResponseSchema` `{ userId, sessionId }`. The actual cookie is set via `Set-Cookie`; the cookie value is **not** in the JSON body.
- **Errors:** `webauthn_verification_failed` (400 — counter regress, signature, etc.) · `no_passkey_for_email` (404) · `rate_limited` (429).
- **Audit:** `passkey_authentication_success` or `passkey_authentication_failure`; `session_revoked` on the prior session if applicable; `session_created` on success; `per_user_dek_derived` in the same transaction.

### `POST /api/auth/logout`
- **Auth:** `session`.
- **Body:** none.
- **Side effects:** `SessionRepository.revoke(sessionId, 'logout')`; if no other active session for this user, `CryptoService.unloadUserDek(userId)`.
- **Response:** `200` `LogoutResponseSchema`.
- **Audit:** `session_revoked`; `per_user_dek_zeroized` if DEK was unloaded.

---

## 4. Plaid

### `POST /api/plaid/link-token`
- **Auth:** `session` (`pccMember` additionally required if `domain==='pcc'`).
- **Body:** `PlaidLinkTokenRequestSchema` `{ domain }`.
- **Side effects:** `PlaidService.mintLinkToken({ userId, domain, products })`.
- **Response:** `200` `PlaidLinkTokenResponseSchema` `{ linkToken, expiresAt }`.
- **Errors:** `plaid_api_error` (502) · `unauthorized` (403 if non-member requests `pcc`).
- **Audit:** `plaid_link_token_minted`.

### `POST /api/plaid/exchange`
- **Auth:** `session` (`pccMember` if `domain==='pcc'`).
- **Body:** `PlaidExchangeRequestSchema` `{ publicToken, domain, institutionId?, institutionName? }`.
- **Side effects:**
  1. `plaid.itemPublicTokenExchange` → `access_token` (in-memory `Buffer` only).
  2. `CryptoService.encrypt({ handle: <pcc|user>, aad: { kind:'item_token', itemId }, domain, plaintext })` → `EncryptedBlob`.
  3. `ItemRepository.create(...)` with `encryptedAccessToken = blob`.
  4. `Buffer.fill(0)` on the access-token buffer.
  5. Audit `plaid_public_token_exchanged` and `plaid_item_added`.
- **Response:** `200` `PlaidExchangeResponseSchema` `{ itemId, plaidItemId }`.
- **Errors:** `invalid_public_token` (400) · `plaid_api_error` (502).
- **Audit:** `plaid_public_token_exchanged` (success/failure) · `plaid_item_added` (success).

### `GET /api/plaid/items`
- **Auth:** `session`.
- **Query:** `?domain=personal|pcc` (optional; defaults to merged-by-scope).
- **Response:** `200` `PlaidItemListResponseSchema` — list of items visible under the caller's `RepoScope` (personal: their own; pcc: all PCC items if the caller is a member; admins: all).
- **Errors:** `unauthorized` (403 for `domain=pcc` non-member).
- **Audit:** none (read-only browsing of metadata).

### `POST /api/plaid/items/remove`
- **Auth:** `session` (`pccMember` if item is `pcc`).
- **Body:** `PlaidItemRemoveRequestSchema` `{ itemId, reason? }`.
- **Side effects:** `PlaidService.removeItem` → call `plaid.itemRemove` upstream, soft-delete (`removedAt = now`), zeroize the `encryptedAccessToken` bytes (`Buffer.fill(0)`; row remains for audit).
- **Response:** `200` `PlaidItemRemoveResponseSchema`.
- **Errors:** `item_not_found` (404 — out-of-scope or genuinely missing).
- **Audit:** `plaid_item_removed`.

---

## 5. Sync (manual trigger)

### `POST /api/sync/run`
- **Auth:** `session`. PCC sync of a PCC item additionally requires `pccMember`.
- **Body:** `SyncTriggerRequestSchema` `{ itemId? }` — if absent, runs the caller's full personal scope and (if PCC member) PCC scope.
- **Side effects:** dispatches to `SyncOrchestrator.syncItem` (single) or queues a one-shot pass for the caller's scope.
- **Response:** `200` `SyncTriggerResponseSchema` `{ outcome: SyncOutcome }`.
- **Errors:** `keybridge_unavailable` (503 if sync worker not connected) · `crypto_unavailable` (503).
- **Audit:** `plaid_sync_started` / `plaid_sync_completed` / `plaid_sync_failed`; `plaid_token_decrypted` per item.

---

## 6. Dashboard

### `GET /api/dashboard/snapshot`
- **Auth:** `session`.
- **Query:** `?domain=personal|pcc`.
- **Side effects:** read-through cache, no writes.
- **Response:** `200` `DashboardSnapshotResponseSchema`:
  ```ts
  { domain, asOf, netWorth: NetWorthResult, monthNet: MonthNetResult, billion: BillionProgressResult, accounts: Account[] (scrubbed) }
  ```
  - Cents are returned as **strings** (via `CentsOutSchema`) to avoid JSON-number precision loss.
- **Errors:** `unauthorized` (403 for `domain=pcc` non-member).
- **Audit:** none (read-only).

### `GET /api/dashboard/series`
- **Auth:** `session`. PCC if `domain=pcc`.
- **Query:** `DashboardSeriesQuerySchema` `{ domain, fromTs, toTs }`.
- **Response:** `200` `DashboardSeriesResponseSchema` — array of `NetWorthSnapshot` summaries within range.
- **Audit:** none.

---

## 7. Admin (owner only)

### `POST /api/admin/enroll`
- **Auth:** `session+owner`. Mirrors the `pnpm admin:enroll` CLI for in-app usage.
- **Body:** `AdminEnrollRequestSchema` `{ email, displayName, role }`.
- **Side effects:** allowlist + placeholder check; mints one-shot enrollment URL token.
- **Response:** `200` `AdminEnrollResponseSchema` `{ enrollmentUrl, expiresAt }`.
- **Errors:** `email_not_allowlisted` (400) · `placeholder_email_rejected` (400).
- **Audit:** `admin_enroll_invoked`.

### `POST /api/admin/revoke`
- **Auth:** `session+owner`.
- **Body:** `AdminRevokeRequestSchema` `{ email }`.
- **Side effects:** revoke active session(s) for that user; revoke all `Passkey` rows for that user (`revokedAt = now`); unload their DEK.
- **Response:** `200` `AdminRevokeResponseSchema` `{ revokedSessions, revokedPasskeys }`.
- **Audit:** `admin_revoke_invoked`.

### `GET /api/admin/audit/verify`
- **Auth:** `session+owner`.
- **Side effects:** `AuditService.verifyChain()`.
- **Response:** `200` `AdminAuditVerifyResponseSchema` `{ verifiedCount, brokenAtSeq?: bigint-as-string }`.
- **Audit:** `admin_audit_verify_invoked`.

### `GET /api/admin/audit/query`
- **Auth:** `session+owner`.
- **Query:** `fromTs?`, `toTs?`, `actorUserId?`, `action?`, `domain?`, `limit?` (max 1000).
- **Response:** `200` array of `AuditEntry` (with `prevHash`/`entryHash` as base64).
- **Audit:** none (admin reading audit is itself audited only at CLI invocation).

---

## 8. IPC keybridge — Unix domain socket

Web process binds `/tmp/greylock-keybridge.sock` (mode `0600`, owner = process uid). Sync worker connects, presents peer credentials, then issues authenticated requests.

### Peer authentication

1. **OS-level:** server calls `getsockopt(LOCAL_PEERCRED)` (macOS) / `getsockopt(SO_PEERCRED)` (Linux fallback). Reject any peer whose `cr_uid` ≠ `getuid()`. Logs `ipc_keybridge_request_denied` with kind `peer_credential_mismatch`.
2. **Application-level handshake (HMAC-SHA-256):**
   - The web process and the sync worker both derive `K = HKDF(MasterKEK, info=utf8('greylock/keybridge/v1'), L=32)` at boot.
   - Server sends a 32-byte random `serverNonce`.
   - Client returns `clientNonce (32B)` and `HMAC-SHA-256(K, serverNonce || clientNonce)`.
   - Server verifies, replies with `HMAC-SHA-256(K, clientNonce || serverNonce)`. Client verifies.
   - On any mismatch, server closes the connection and audits `ipc_keybridge_request_denied` with kind `auth_failed`.

The handshake proves the client knows the same `MasterKEK` without revealing it. Since the sync worker also boots from Keychain, this works without sharing a separate secret.

### Wire format

Newline-delimited JSON, lines ≤ 16 KiB. Binary payloads are base64-encoded.

```text
→ HELLO       {"v":1, "pid":<int>, "uid":<int>}
← HELLO_OK    {"v":1, "serverNonce":"<base64-32B>"}
→ AUTH        {"clientNonce":"<base64-32B>", "hmac":"<base64-32B>"}
← AUTH_OK     {"hmac":"<base64-32B>"}    (or AUTH_DENY {"reason":"..."} → close)

(steady state, request/response by id)
→ REQUEST     {"id":"<uuid>", "method":"requestDek", "params":{"userId":"<UserId>","sessionId":"<SessionId>"}}
← RESPONSE    {"id":"<uuid>", "ok":true,  "result":{"handle":{"kind":"user","userId":"...","version":2}, "dekB64":"<base64-32B>"}}
                  or {"id":"<uuid>", "ok":false, "error":{"kind":"session_invalid"|"dek_unavailable"|...}}

→ REQUEST     {"id":"<uuid>", "method":"requestDek", "params":{"kind":"pcc"}}
← RESPONSE    {"id":"<uuid>", "ok":true,  "result":{"handle":{"kind":"pcc","version":3}, "dekB64":"<base64-32B>"}}

→ REQUEST     {"id":"<uuid>", "method":"releaseDek", "params":{"handle":{"kind":"user",...}}}
← RESPONSE    {"id":"<uuid>", "ok":true}

→ REQUEST     {"id":"<uuid>", "method":"ping"}
← RESPONSE    {"id":"<uuid>", "ok":true, "result":{"chainHead":"<base64-32B>"}}
```

### Error semantics

| Code | When |
|---|---|
| `socket_unavailable` | Worker can't connect; web process is down. Worker logs and skips the cycle. |
| `peer_credential_mismatch` | OS peer creds wrong. Fatal — connection is closed. Audited. |
| `auth_failed` | HMAC handshake failed. Fatal. Audited. |
| `session_invalid` | `requestDek({user})` for a userId whose session is revoked/expired or whose DEK isn't currently loaded. Worker skips this user this cycle. |
| `dek_unavailable` | `requestDek({pcc})` before `CryptoService.hasPccDek()` is true. Should never happen post-boot. |
| `protocol_error` | Malformed JSON, oversized line, unknown method. Fatal. |
| `timeout` | Request exceeded 5s without response. |

### Lifetime + recovery

- Created by `lib/runtime/boot.ts` after Master KEK unwrap succeeds.
- `boot.ts` `unlink()`s any stale socket before binding.
- On `pnpm dev` shutdown: `lib/runtime/shutdown.ts` closes the socket and audits `master_kek_unloaded` + `pcc_dek_zeroized`.
- If the web process dies mid-sync: worker's next `requestDek` returns `socket_unavailable`; worker zeroizes any DEK it had borrowed, releases handles, and waits for the socket to come back.

---

## 9. Admin CLI

All scripts under `scripts/` and exposed via `pnpm admin:*`. They run as `actorKind: 'admin_cli'`, write to the audit log, and exit non-zero on any failure.

### `pnpm admin:enroll <email> [--name "Display Name"] [--role owner|member]`
1. Asserts `email ∈ ALLOWED_EMAILS` and `email !== 'cade-placeholder@greylock.invalid'`.
2. Inserts/updates `User` row.
3. Mints a one-shot enrollment URL with a 32-byte token, 30-min TTL. Token is hashed before storage; URL contains the plaintext.
4. Prints the URL to stdout. **Never logs to a file** — operator copies-pastes into a private channel for the recipient.
5. Audit: `admin_enroll_invoked`.

### `pnpm admin:revoke <email> [--reason "..."]`
1. Revokes all active sessions for the user (`reason='admin_revoke'`).
2. Marks all `Passkey` rows for the user as `revokedAt = now`.
3. Calls `CryptoService.unloadUserDek` if loaded.
4. Audit: `admin_revoke_invoked`.

### `pnpm admin:revoke-all [--reason "..."]`
1. Revokes every active session.
2. `CryptoService.shutdown()` — clears all in-memory DEKs (Master, PCC, all per-user).
3. Audit: `admin_revoke_all_invoked`.
4. **Does NOT** revoke passkeys — use `:revoke <email>` per-user for that.

### `pnpm admin:rotate-master`
1. Prompt operator for the **new** master passphrase (TTY echo off). Confirm by re-entry.
2. Read **current** master passphrase from Keychain → derive current Master KEK.
3. Generate new salt; derive new Master KEK from new passphrase.
4. Decrypt PCC DEK under current Master KEK → re-encrypt under new Master KEK → insert new `PccKeyWrap` row (new `version`), retire old (`retiredAt = now`).
5. **Re-encrypt every PCC `Item.encryptedAccessToken`** under the new wrap version (the AAD references `masterKekVersion`, so old ciphertexts would no longer verify).
6. Update macOS Keychain via `security add-generic-password -U`.
7. Atomic: any failure rolls back via `Prisma.$transaction` and leaves the previous wrap in use.
8. Audit: `admin_master_rotation_started` + `admin_master_rotation_completed` (or `admin_master_rotation_failed`).

### `pnpm admin:audit-verify`
1. `AuditService.verifyChain()` — walks the table seq-ascending, recomputes every `entryHash`, returns the seq of any mismatch.
2. Prints `OK seq=<N>` or `BROKEN at seq=<N>`.
3. Audit: `admin_audit_verify_invoked`.

---

## 10. Cross-cutting concerns

### Audit-log emit points (route → action)
Every route's audit emit is listed in §2-7. Universal rules:
- Validation failures audit with `outcome=failure` and a sanitized `details` summary (no payload echo).
- Authorization failures (out-of-scope reads) audit with `outcome=denied`.
- Storage failures audit `outcome=failure` and surface as 500.

### Sanitizer guarantees (`lib/audit/sanitizer.ts`)
Detail payloads pass through a deny-list before persistence:
- Substring scan for: `access_token`, `refresh_token`, `link_token`, `secret`, `passphrase`, `password`, `cookie`, `dek`, `kek`, hex/base64 strings ≥ 64 chars.
- Any hit → `sanitizer_rejected_payload` and the route returns 500 with no leakage.

### Rate limits
- Auth begin/complete: 5 attempts / 15 min / `(IP, email)` bucket. Bucket lives in `RateLimitBucket` table.
- Plaid `link-token` and `exchange`: 30 / hour / userId.
- Sync trigger: 4 / minute / userId.
- Health: 60 / min / IP.
- All rate-limit trips audit `rate_limit_tripped`.

### Headers
Set globally by `next.config.mjs` (Phase 0). Phase 5 will add the full CSP. See `docs/ARCHITECTURE.md` §8 for the provisional CSP.
