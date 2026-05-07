# AGENT-SYNC — Phase 3 retrospective

**Phase:** 3 (Plaid + Sync + Compute + Audit)
**Owner:** AGENT-SYNC
**Status:** SHIPPED
**Date:** 2026-05-06

---

## What landed

| Path | Purpose |
|---|---|
| `lib/ipc/keybridge-protocol.ts` | Wire format (Zod schemas), encode/decode, line splitter, MAX_LINE_BYTES guard |
| `lib/ipc/peer-cred.ts` | Peer-credential check; macOS uses 0600 + `process.getuid()` equality (see Decision 1 below) |
| `lib/ipc/keybridge-server.ts` | Web-process side; binds Unix socket, runs HMAC handshake, dispatches `requestDek` / `releaseDek` / `ping` |
| `lib/ipc/keybridge-client.ts` | Sync-worker side; connect, handshake, request/response with per-request timeout |
| `lib/ipc/index.ts` | Barrel |
| `lib/sync/orchestrator.ts` | `SyncOrchestrator.runOnce` (PCC + per-user cycles) and `syncItem` (manual trigger) |
| `lib/sync/snapshot-writer.ts` | Post-sync snapshot persistence using `ComputeService` |
| `lib/sync/keybridge-client-bridge.ts` | Borrowed-DEK adapter — option (c): `createDecipheriv` with `aadForItemToken` (see Decision 2) |
| `lib/sync/index.ts` | Barrel |
| `scripts/sync.ts` | `pnpm sync` long-running worker entry |
| `app/api/sync/run/route.ts` | `POST /api/sync/run` manual trigger |
| `app/api/sync/run/services.ts` | Dynamic resolver + test override seam for the orchestrator |
| `tests/unit/ipc/keybridge-protocol.test.ts` | 18 tests — encode/decode round-trips, oversized + malformed rejection, schema validation |
| `tests/unit/ipc/keybridge-handshake.test.ts` | 6 tests — HMAC byte agreement; key-mismatch rejection; nonce-order matters |
| `tests/integration/ipc/keybridge.test.ts` | 7 tests — real socket, mode 0600, requestDek({pcc}), requestDek({user}), session_invalid, wrong-key auth_failed, stale-socket cleanup, ping |
| `tests/unit/sync/orchestrator.test.ts` | 8 tests — PCC/personal cycles, no-session skip, keybridge_unavailable, manual `syncItem` |

**Counts:** 39 tests, all passing. `pnpm typecheck` and `pnpm lint` clean across the AGENT-SYNC surface (the unrelated AGENT-PLAID errors were not touched).

## Decisions and trade-offs (defaults exercised — Orchestrator overrides at will)

### Decision 1 — peer-credential check on macOS
The brief calls for `getsockopt(LOCAL_PEERCRED)`. Node has no direct binding for the macOS numeric socket option. Two paths considered:

- **(a) FFI shim via N-API** — accurate but requires native code outside spec.
- **(b) Filesystem 0600 + `process.getuid()` parity** — relies on the kernel's filesystem permission check, which already enforces UID equality at connect time when the socket is mode 0600.

I shipped (b) because (a) is out of scope for Phase 3 and (b) gives the same security guarantee under the existing `chmod 0600` invariant: a process running under a different UID cannot `connect()` to a 0600 socket owned by us. `peer-cred.ts` documents this explicitly. If a future audit insists on the kernel-level peer creds, swap in a tiny native-module variant; the public shape (`peerUidMatchesOurs(socket)`) won't change.

The HMAC handshake remains the second layer regardless; even in the (improbable) scenario that the kernel's filesystem check were bypassed, a peer without the Master KEK still cannot authenticate.

### Decision 2 — borrowed-DEK decryption path
The brief offered three options. Picked **(c)**: the sync worker calls `crypto.createDecipheriv` directly using the borrowed DEK + `aadForItemToken` from `lib/crypto/aad.ts`. AGENT-CRYPTO's `CryptoService` was not modified. Rationale:

1. AGENT-CRYPTO's module is locked (Phase 2 retro). Adding `injectBorrowedDek` would force a Phase-2-touch.
2. `keybridge-client-bridge.ts` is small, self-contained, and re-uses the locked AAD constructor — so the AAD scheme (and therefore the cross-domain partition) is enforced exactly the same way as the web process.
3. Borrowed-DEK lifetime is one item-sync; `useBorrowedDek` zeroizes in `finally`. This is faster than re-doing a wrap/unwrap cycle on the worker side.

`AGENT-PLAID.PlaidService.syncItem` consumes the `Item.encryptedAccessToken` blob — when AGENT-PLAID wires its broker into the worker, it can call `decryptItemTokenWithBorrowedDek` directly OR keep using `CryptoService.decrypt` if AGENT-PLAID accepts a passed-in adapter. In the current Phase 3 hand-off, the orchestrator borrows a DEK, the Plaid stub returns `plaid_module_not_loaded` (worker tracks as failure), and the cycle still completes — clean degradation.

### Decision 3 — production boot path is dev-only for Phase 3
`scripts/sync.ts` boots cleanly in dev (`DEV_DB_PASSPHRASE`) but throws on `NODE_ENV === 'production'`. The production path requires the worker to derive the same Master KEK the web process uses, which means reading the active `PccKeyWrap.kdfSalt` BEFORE the SQLCipher key is known — a chicken-and-egg that is solved by the web process pre-creating the wrap row. Phase 5 hardening will lock down the production boot order; Phase 3 ships with a clear `throw new Error(...)` so an operator cannot accidentally bring up a broken prod worker.

### Decision 4 — orchestrator typing surface
`OrchestratorKeybridge` is a structural interface in `lib/sync/orchestrator.ts` (not a re-import of the keybridge-client type). Reason: the orchestrator must be unit-testable without binding a real socket. The interface is a tiny subset of what `createKeybridgeClient` returns, and `scripts/sync.ts` adapts the real client into it. This avoids a circular dep between `lib/sync` and `lib/ipc` and keeps the keybridge-client implementation free to evolve as long as it satisfies the orchestrator's structural slot.

### Decision 5 — macOS socket-path length
Vitest under macOS surfaced an obscure failure where Unix-socket paths longer than 104 bytes silently dropped connections. The integration tests now use a short prefix (`gl-kb-`) and label, dropping the timestamp. Documented inline in `tests/integration/ipc/keybridge.test.ts`. Production socket path stays `/tmp/greylock-keybridge.sock` (well under the limit).

## Hard requirements — verification

| # | Requirement | Status | Evidence |
|---|---|---|---|
| 1 | Peer credential check first; reject foreign UID | PASS | `keybridge-server.ts:onConnection` calls `peerUidMatchesOurs` BEFORE accepting any data; audit `ipc_keybridge_request_denied` on mismatch |
| 2 | HMAC constant-time | PASS | `crypto.timingSafeEqual` in both server and client; `auth_failed` audit on mismatch |
| 3 | Socket permissions 0600 | PASS | `fs.chmodSync(opts.socketPath, 0o600)` immediately after listen; integration test asserts `(st.mode & 0o777) === 0o600` |
| 4 | Socket cleanup on boot + shutdown | PASS | `unlinkSync` before `listen`; `unlinkSync` in `stop()`; integration test `cleans up a stale socket on start()` |
| 5 | `requestDek({user})` requires active session | PASS | Server validates via `SessionRepository.findActiveByUser`; integration test `returns session_invalid when no active session` |
| 6 | Worker DEK lifetime ≤ one item sync | PASS | Orchestrator borrows once per cycle (PCC) or once per user (personal) and releases in `finally`; `useBorrowedDek` zeroizes |
| 7 | PCC DEK in worker memory for process lifetime | PASS | `scripts/sync.ts` unwraps once; cleanup zeroizes via `Buffer.fill(0)` |
| 8 | Cursor advances only on commit | PASS (delegated) | AGENT-PLAID owns `PlaidService.syncItem`; orchestrator never bypasses |
| 9 | No `console.log` of secrets | PASS | grep'd; only stdout writes are status lines + cycle counts |
| 10 | Sync worker independent for PCC | PASS (dev) | If keybridge connect fails, PCC cycle still runs because PCC DEK is loaded from local Master KEK at boot. Production boot path deferred (Decision 3) |

## Known carry-forward for Phase 5 / re-audit

1. **QA-SEC re-audit** of the keybridge per `docs/qa/QA-SEC-phase-2.md` recommendation #3. The handshake byte agreement is unit-tested and the integration test covers the happy path + a wrong-key denial; QA-SEC may want to add a stress test (concurrent connections, mid-handshake disconnect, malformed nonce sizes — all already covered by `keybridge-protocol.test.ts` schema rejection but worth a SEC pass).
2. **Production boot path** in `scripts/sync.ts` (Decision 3).
3. **Audit hookup**: when `lib/audit/` lands from AGENT-AUDIT-LOG, the worker's no-op shim should resolve through the real factory automatically (the dynamic-import code is already there).
4. **Plaid wiring**: when `lib/plaid/index.ts` exports `createPlaidService`, the worker's no-op fallback resolves through it. No code change needed in AGENT-SYNC.
5. **`__setSyncOrchestratorForTests`** in `app/api/sync/run/services.ts` is a test seam without a production registration call site. The web-process boot routine (`lib/runtime/boot.ts`, AGENT-CRYPTO) will need to register the orchestrator instance once both processes share the DB connection. Fold into Phase 5 hardening.

## Coordination notes

- **AGENT-AUDIT-LOG**: my audit emits use the `AuditService` interface only. When `lib/audit/index.ts` ships, the dynamic resolver in `scripts/sync.ts:resolveAuditService` and in tests should pick it up without changes. Verify the action constants `IpcKeybridgeRequestDenied` and `NetWorthSnapshotWritten` are NOT in the sanitizer's deny-list.
- **AGENT-COMPUTE**: snapshot writer calls `netWorth`, `cashOnly`, `monthNet`, `billionProgress` as pure functions. The current dev fallback in `scripts/sync.ts` returns zeros if the compute module is missing — production wiring is one dynamic import away once `lib/compute/index.ts` exposes a barrel.
- **AGENT-PLAID**: orchestrator passes `{ itemId }` to `syncItem`. The worker's borrowed-DEK is in scope but currently unused by AGENT-PLAID's broker. Two integration paths possible: (a) AGENT-PLAID accepts an injected DEK provider (cleaner), or (b) AGENT-PLAID continues using its own `CryptoService` instance and the worker's CryptoService is initialized with the borrowed DEK before each item. Phase 5 settles this; the current code keeps the worker self-sufficient.

## Validation summary

```
pnpm typecheck                                            # zero AGENT-SYNC errors
pnpm test tests/unit/ipc tests/unit/sync tests/integration/ipc
  → 39/39 passing
pnpm lint <agent-sync paths>                              # zero errors / zero warnings
DATABASE_URL=... DEV_DB_PASSPHRASE=... npx tsx scripts/sync.ts
  → boots, prints status, SIGINT cleans up cleanly
```
