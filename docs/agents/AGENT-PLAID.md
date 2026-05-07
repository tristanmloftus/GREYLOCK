# AGENT-PLAID — Phase 3 retrospective

**Status:** complete. Plaid SDK wired, token broker hardened, four routes shipped, 39 dedicated tests green (424 across the whole suite).

---

## What landed

| Path | Purpose |
|---|---|
| `lib/plaid/client.ts` | Plaid SDK singleton; reads `PLAID_CLIENT_ID/SECRET/ENV/PRODUCTS/COUNTRY_CODES`; secret never logged. |
| `lib/plaid/token-broker.ts` | The only place a plaintext access token materializes. Reads admin-scope item, decrypts with AAD bound to (domain, itemId, keyVersion), invokes `use(token)` for one Plaid call, `Buffer.fill(0)` in `finally`. Audits `plaid_token_decrypted` (success/failure) with itemId only. |
| `lib/plaid/service.ts` | `PlaidService`: `mintLinkToken`, `exchangePublicToken`, `syncItem`, `refreshBalances`, `removeItem`. Plaid SDK errors mapped to `plaid_api_error{httpStatus, errorCode}` with constant message — no SDK-string echo. |
| `lib/plaid/mappers.ts` | Plaid floats → `Cents` bigint (half-away-from-zero); sign-convention preserved (Plaid: `+`=outflow); USD-only at v0.1. |
| `lib/plaid/index.ts` | Factory + barrel. |
| `app/api/plaid/{link-token,exchange,items,items/remove}/route.ts` | Four routes, byte-for-byte against `docs/API.md` §4. Session validation + PCC membership gate when `domain==='pcc'`. |
| `tests/unit/plaid/{mappers,token-broker}.test.ts` | 34 unit tests covering Cents edge cases, sign preservation, broker zeroize on success / use-throw / decrypt-fail, AAD per domain, admin-scope reads. |
| `tests/integration/plaid/service.test.ts` | 5 integration tests against real SQLCipher DB + real CryptoService + mocked Plaid SDK: encrypted blob round-trip, cross-domain substitution rejection, syncItem cursor advance/rollback, removeItem soft-delete + audit. |
| `lib/runtime/services-registry.ts` | Added `getPlaidService()` and `getFullRepos()` accessors (justification below). |

## Verification (fresh output)

- `pnpm typecheck` → exit 0.
- `pnpm test tests/unit/plaid/ tests/integration/plaid/` → 39/39 pass (~7s).
- `pnpm test` (full suite) → **424/424 pass**.
- `pnpm lint lib/plaid app/api/plaid tests/unit/plaid tests/integration/plaid` → **0 errors**, 1 warning (benign object-injection on a `Record<string, ...>` literal lookup in `mappers.ts:108`).
- Manual grep for `access_token | link_token | public_token | refresh_token`: zero plaintext-token occurrences outside `service.ts:exchangePublicToken` and the broker's `use` callbacks (which call the Plaid SDK).

## Hard rules audit (per brief §Hard requirements)

1. **Plaintext access token NEVER persisted.** ✅ Only materializes in two spots: (a) inside `exchangePublicToken` between the SDK call and `Buffer.fill(0)` in `finally`, (b) inside `withDecryptedToken` for the duration of `use()`. Both zero-fill the `Buffer` in `finally` regardless of throw.
2. **Encryption happens BEFORE the row is "live".** ✅ See deviation note below.
3. **Cursor advances only on commit.** ✅ `syncItem` calls `applyPlaidSync` then `updateSyncCursor(success)` sequentially; on any failure path `updateSyncCursor(error)` is called with the **prior** cursor value, so the cursor can't accidentally advance.
4. **Audit on every Plaid call.** ✅ `plaid_link_token_minted`, `plaid_public_token_exchanged`, `plaid_item_added`, `plaid_token_decrypted`, `plaid_sync_started`, `plaid_sync_completed`, `plaid_sync_failed`, `plaid_item_removed`. Counts only — no transaction amounts, no merchant names, no transaction IDs in `details`. Verified by integration test.
5. **No `console.log` of tokens.** ✅ Grepped clean.
6. **No SDK error text in API responses.** ✅ `mapPlaidSdkError` extracts only `httpStatus` + `errorCode`; the route layer surfaces `'plaid call failed'` as a constant message.
7. **PCC route gating.** ✅ Routes verify `PccMembershipRepository.isActiveMember` before service calls when `domain==='pcc'`. The service re-checks for defense-in-depth.
8. **Repository scope.** ✅ `personal:userId` for personal items, `pcc:memberOfUserId` for PCC items, `admin` only inside the broker / sync / rewriteEncryptedToken.
9. **`Result<T, ...>` returns; no throws across module boundaries.** ✅ All public methods.
10. **No mocked Plaid responses in production code.** ✅ Test fixtures live only under `tests/`.

## Coordination decisions / deviations

### 1. `Item.id` generation: insert + rewrite (instead of pre-generated cuid)

**Deviation from brief.** The brief specifies "Generate the new `Item.id` ahead of time (cuid) so AAD can bind to it" and uses `ItemRepository.create(scope, {id: itemId, …})`. The `ItemRepository.create` signature in `lib/types/services.ts` does **not** accept `id` (Prisma's `@default(cuid())` assigns it server-side). Since AGENT-PLAID's Must-not-touch list includes `lib/types/*` and `lib/db/*`, I cannot extend the repo contract.

**What I did instead.**
- Insert the `Item` row with a length-correct **placeholder zero blob** (1 byte version, 1 byte domain_tag, 12 zero nonce, 0-byte ciphertext, 16 zero tag — passes the Bytes column constraint, cannot decrypt under any key).
- DB assigns the real `Item.id` (cuid).
- Encrypt the actual access token with AAD bound to that real id (`personal:itemtoken:<itemId>:<userDekVersion>` or `pcc:itemtoken:<itemId>:<masterKekVersion>`).
- `rewriteEncryptedToken({kind:'admin'}, {id, newBlob})` swaps the placeholder for the AAD-bound ciphertext.
- `Buffer.fill(0)` on the plaintext buffer in `finally`.

**Security implication.** Between step 1 (insert) and step 5 (rewrite) the row's `encryptedAccessToken` is a no-op blob — there is **never** a plaintext token in the DB. If steps 4–5 fail mid-flow, the row exists with an unusable blob, the next sync fails with `crypto_decrypt_failed`, and the operator removes the item via `POST /api/plaid/items/remove`. We audit the partial-failure outcome.

**Recommendation for QA-SEC re-audit.** Confirm: (a) the placeholder blob can never be confused for a real ciphertext (it has zero tag bytes; GCM open will always fail); (b) the only persistent state for this item between insert and rewrite is the placeholder; (c) the audit trail surfaces partial-failure clearly. If this is unacceptable, the cleanest fix is for AGENT-DB + AGENT-ARCH to extend `ItemRepository.create` input with an optional `id: ItemId` field — at which point this file's `exchangePublicToken` can collapse into the brief's original 1-step shape.

### 2. `getPlaidService()` accessor in `services-registry.ts`

The brief explicitly permits this with retro justification. The accessor follows the same lazy-singleton + dynamic-import pattern as `getCryptoServiceLazy` / `getRepos` / `getAuditService`. Two helpers added:
- `getPlaidService()` — wires `PlaidApi` + crypto + full repos + audit and returns `PlaidService`.
- `getFullRepos()` — returns the booted-DB bundle including item / account / transaction / pcc-membership / pcc-key-wrap repos. The route handlers under `app/api/plaid/*` use this for membership checks. AGENT-SYNC's orchestrator will reuse it.

The existing `getRepos()` returns only the auth-scoped subset (`userRepo`, `passkeyRepo`, `sessionRepo`, `rateLimitRepo`, `wrappedDekReader`); since the `ResolvedRepos` interface was authored when only AGENT-AUTH existed, and the brief forbids modifying `lib/runtime/services-registry.ts`'s existing surface, I added `getFullRepos()` as a parallel accessor rather than expanding `ResolvedRepos`. Phase 5 may collapse the two.

### 3. Broker contract: catch SDK errors inside `use()`

The brief says "On `use` throwing, finally still zeroes" — the broker's `Buffer.fill(0)` runs in `finally` regardless. However, when an SDK call inside `use()` throws (e.g. a 502 from Plaid), the broker re-throws past its `Result` boundary, breaking the "no throws across module boundaries" invariant for the service.

**Resolution.** The service's three borrow sites (`syncItem`, `refreshBalances`, `removeItem`) wrap their `use(token)` body with an internal try/catch that maps SDK errors to a typed `Either` result. The broker still re-throws unexpected non-SDK errors (defensive); the service catches those too and surfaces them as `plaid_api_error{httpStatus:0, errorCode:'unexpected'}`. This keeps the broker simple while preserving the service's `Result` discipline.

### 4. Test scrypt parameters

Integration tests pay the production scrypt cost (~250ms/test for `N=2^17`) once per test, because `CryptoService.initializeFromKeychain` re-derives the Master KEK and the seed-time KEK must match. Total wall time: ~6s for 5 integration tests. Deferring optimization until QA-TEST flags it.

## Defaults documented (autonomous decisions)

- `clientName` for `linkTokenCreate`: `'Greylock'` (production) / `'Greylock-Test'` (tests).
- `language: 'en'`, `country_codes` from `PLAID_COUNTRY_CODES` (default `['US']`).
- `webhook` field omitted for v0.1 (per brief).
- `count: 500` per `transactionsSync` page.
- `Plaid-Version` header pinned to `'2020-09-14'` (matches the SDK's bundled OpenAPI version).
- USD-only at v0.1 (mappers throw on non-USD currency code).
- Cents rounding: half-away-from-zero (matches POSIX `round()`, stable under sign flips).

## QA-SEC carry-forward (for the Phase-3 boundary re-audit)

Per `docs/qa/QA-SEC-phase-2.md` recommendation #2 ("Re-audit Plaid token-broker"):

- ✅ `Buffer.fill(0)` after every `withDecryptedToken` call — verified by `tests/unit/plaid/token-broker.test.ts` ("zeroizes buffer when use callback throws").
- ✅ AAD `kind:'item_token'` bound at both encrypt and decrypt — verified at `service.ts:exchangePublicToken` step 4 and `token-broker.ts` step 4.
- ✅ No `console.log(token)` even at debug — grep clean.
- ⚠️ See deviation #1 above re: insert+rewrite vs. pre-generated cuid. Recommend QA-SEC sign off on the placeholder-blob pattern or push for the repo contract extension.

## Files that were modified outside `lib/plaid/` and `app/api/plaid/`

- `lib/runtime/services-registry.ts` — added `PlaidService` to imports/overrides; new `getPlaidService()` and `getFullRepos()` accessors. The pre-existing `RegistryOverrides` mechanism (M-3 from QA-SEC Phase 2) is still production-unguarded; AGENT-PLAID did not address that — it's tracked for Phase 5.

No other agent-owned modules were touched.
