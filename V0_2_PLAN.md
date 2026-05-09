# TerminalFinance v0.2 — Evolution Plan

**Status:** DRAFT (uncommitted). Rory must answer the open questions in §i before any phase dispatches.
**Author:** Claude (planning session, 2026-05-09).
**Source of truth:** Tristan's v0.1 on `origin/main`, Rory's `master` (TS prototype + `AUDIT.md` + `docs/`). Mine TS for patterns; do not bring TS code into the C++ tree.

This plan is what the v0.2 build-out works from. Where it makes a decision, the orchestrator acts on it. Where it surfaces a `[NEEDS RORY DECISION]`, the orchestrator stops and asks before dispatching any work that depends on the decision.

---

## a) State of v0.1

What's actually shipped on `origin/main`:

- Build: C++20, CMake (FetchContent for FTXUI / nlohmann/json / GoogleTest). Links Windows-only libs (`crypt32`, `winhttp`, `advapi32`). Tristan develops on Windows; macOS does not currently build.
- TUI: FTXUI app in `src/main.cpp` with four tabs (Dashboard, Accounts, Transactions, Budget). Uses an "LED blue" color scheme defined in `src/views/ViewCommon.h`.
- Models: `Entity` (Individual/LLC/Corporation/Partnership/Trust/Other), `Account` (Checking/Savings/CreditCard/Investment/Cash/Other), `Transaction` (signed amount; negative = expense), `Category`, `Budget`. All in-memory `std::vector` collections on a `DataStore` singleton-ish class.
- Storage: `JsonStorageService` writes the entire `DataStore` as one JSON file (`get_storage_path()` from `ConfigManager`). Plaintext on disk. Single global file, no per-user separation.
- Plaid: `IPlaidService` interface + `StubPlaidService` (returns canned values) + a real `PlaidService.cpp` implementation. **Critical**: the interface takes the access token by value as a `std::string` argument (`get_transactions(access_token, ...)`). No token-broker pattern. The plaintext token is also persisted directly on `Account.plaid_access_token` in memory.
- Security: `SecurityService` is **Windows-only** by construction — `#include <windows.h>` + `<wincred.h>`, uses DPAPI (`CryptProtectData`/`CryptUnprotectData`) for envelope encryption and stores ciphertexts in HKCU registry at `Software\TerminalFinance\Tokens` keyed by `account_id`. Per-user, no UI prompt, automatic on Windows login. There is no macOS counterpart.
- Discovery: `DiscoveryService` (singleton). Maps transaction descriptions to supplier tickers via keyword match (`SupplierMapping`); computes MoM spend velocity per category (`VelocityResult`). Headline feature ("Shovel Intelligence"). Already wired into `DashboardView::render`.
- Consolidation: `ConsolidationService` (singleton). Hash-based dedup for Plaid transactions (`transaction_id|amount|date`). Aggregates `TotalLiquidity` across account types. Looks reasonable; not deeply audited.
- Config: `ConfigManager` (singleton) reads `.env` for Plaid creds + storage path. Standard pattern.
- Logging: `Logger` (singleton, mutexed), file-based at `terminalfinance.log`.
- Tests: one file (`tests/test_datastore.cpp`), ~7 GoogleTest cases on DataStore CRUD + validation. No coverage on services. No FTXUI snapshot harness.
- Proposals: 7 FTXUI components staged in `proposals/` — `consolidation_ui`, `ui_category_trends`, `ui_net_worth`, `ui_shovel_intelligence`, `ui_shovel_score`, `ui_sync_status`, `ui_updates` — drafted but **not** referenced from `main.cpp` or built into the executable.

Concrete v0.1 bugs / gaps the plan addresses:

1. **Plaintext access tokens in memory** on `Account.plaid_access_token`. Anyone with a heap dump of the running process gets every Plaid token.
2. **No token-broker pattern.** Callers pass the token by value into `PlaidService::get_transactions(...)`. The token's lifetime is tied to whatever caller frame holds it.
3. **Plaintext JSON on disk** for everything except access tokens. `Account.balance`, transaction descriptions, entity tax IDs — all in the open.
4. **macOS does not build.** Hard-coded Windows includes in `SecurityService`.
5. **No audit log.**
6. **No auth.** Anyone with the binary and the local data file IS the user.
7. **Shovel data is hard-coded** in `DiscoveryService` (`SupplierMapping` is a static list). v0.2 needs a refresh mechanism.
8. **No off-machine backend.** Everything is local to one box.

What v0.1 has right and v0.2 should not touch:

- The Entity model. "Personal and business" generalizes cleanly.
- The DI pattern via `ServiceContainer`.
- The four-tab TUI spine.
- Discovery + Consolidation as named services.
- The `.env`-driven config approach.
- The signed-amount Transaction convention.

---

## b) Module Map

Format: `<file>` — `KEEP` / `EXTEND` / `REPLACE` / `ADD` — one-line rationale.

### `src/models/`

- `Entity.h` — KEEP. Multi-entity model is correct and stays canonical.
- `Account.h` — EXTEND. Remove the `plaid_access_token` string field; replace with token lookup via `account_id` against `SecurityService`. Add an `is_plaid_linked` bool.
- `Transaction.h` — KEEP.
- `TransactionFilter` (in `Transaction.h`) — KEEP.
- `Category.h` — KEEP unless §c forces a schema change.
- `Budget.h` — KEEP unless §c forces a schema change.
- `DataStore.h/.cpp` — EXTEND. Today it's a fat in-memory CRUD store with JSON load/save. v0.2: keep the in-memory side (used by views), but `load`/`save` route through the new `BackendClient` rather than `JsonStorageService` directly. Same interface from the views' perspective.

### `src/services/`

- `ServiceContainer.h` — EXTEND. Add slots for `BackendClient`, `AuthService`, `CryptoService`, `AuditLogClient` (server-side audit log; client just appends).
- `StorageService.h/.cpp` — EXTEND. The `IStorageService` interface stays. `JsonStorageService` stays for offline / dev / migration use. `ADD` a new `RemoteBackendStorageService` that implements `IStorageService` against the server.
- `PlaidService.h/.cpp` — EXTEND. Two big changes: (1) the interface gains a `withDecryptedToken<T>` template (or moral equivalent — see §e); existing methods that take a plaintext token become private and only the broker calls them. (2) Network calls move behind an injected `IHttpClient` so the same library powers Plaid + the backend client.
- `SecurityService.h/.cpp` — REPLACE. Today it's Windows-only DPAPI + registry. v0.2: a cross-platform `ISecretStore` abstraction with two implementations: `DpapiSecretStore` (Windows, lifts most of v0.1 verbatim) and `KeychainSecretStore` (macOS, `Security.framework`). Same interface; selection at compile time via CMake target. Also add envelope encryption helpers (libsodium, see §e) and `withZeroized<T>` RAII for plaintext-buffer scope.
- `DiscoveryService.h/.cpp` — EXTEND. Three changes: (1) supplier map becomes loadable from a JSON file instead of hard-coded; (2) MoM velocity gets unit-tested with realistic merchant strings; (3) add a `refresh_supplier_data()` hook that pulls an updated mapping from the backend (not Plaid) so we can update without binary releases.
- `ConsolidationService.h/.cpp` — KEEP, plus tests. Add a cross-account-same-transaction test case. The dedup hash (`transaction_id|amount|date`) is correct for the single-account case; cross-account needs verification.
- `IHttpClient.h` — ADD. Thin wrapper interface around libcurl (or whatever §c locks in). Owns timeout + TLS verification policy.
- `BackendClient.h/.cpp` — ADD. Handles transport to the off-machine server: auth headers, JSON serialization, error mapping. PlaidService and StorageService both consume it for network calls.
- `AuthService.h/.cpp` — ADD. Manages user identity, login/logout, session token lifecycle. Specifics depend on §d.
- `AuditLogClient.h/.cpp` — ADD. Client-side append-only emitter. Talks to backend; the actual hash chain lives on the server.

### `src/utils/`

- `ConfigManager.h/.cpp` — EXTEND. Add `get_backend_url()`, `get_user_email()`, `get_session_storage_path()`. `.env` keys: `TF_BACKEND_URL`, `TF_USER_EMAIL`, `PLAID_CLIENT_ID` (server-side now — clients don't see Plaid creds).
- `Logger.h` — KEEP. Already cross-platform.
- `SyntheticGenerator.h/.cpp` — KEEP. Useful for tests + demo mode.
- `Validator.h/.cpp` — KEEP and extend in §h.

### `src/views/`

- All views (`DashboardView`, `AccountsView`, `TransactionsView`, `BudgetView`, `ViewCommon.h`) — KEEP. They consume `DataStore` directly and don't know about persistence — perfect.
- `ViewCommon.h` — KEEP. Color palette + currency helpers.

### `proposals/` → `src/views/`

These move into `src/views/` and get wired into `main.cpp` in Phase 5:

- `ui_net_worth.{h,cpp}` — promote, wire as a Dashboard panel.
- `ui_category_trends.{h,cpp}` — promote, wire as a Dashboard panel.
- `ui_shovel_intelligence.{h,cpp}` — promote, wire as a top-level Discovery panel on Dashboard.
- `ui_shovel_score.{h,cpp}` — promote, wire as a Dashboard widget.
- `ui_sync_status.{h,cpp}` — promote, wire into the header (status of last backend sync).
- `ui_updates.{h,cpp}` — promote; nature unclear from filename. Inventory in Phase 5 before deciding.
- `consolidation_ui.{h,cpp}` — promote, wire as an admin-style view (manual dedup review).

### `tests/`

- `test_datastore.cpp` — KEEP. Extend to ~30 cases.
- ADD `test_security_service.cpp`, `test_plaid_service.cpp`, `test_consolidation_service.cpp`, `test_discovery_service.cpp`, `test_audit_log.cpp`, `test_auth.cpp`, `test_http_client.cpp`, `test_backend_client.cpp`.
- ADD `test_views_snapshot.cpp` — FTXUI snapshot harness; renders each view to a string and diffs against a checked-in golden file.

### Build / CI

- `CMakeLists.txt` — EXTEND. Platform-conditional library linking. New deps: libcurl, libsodium, sqlite3 (+ SQLCipher for the backend). vcpkg manifest on Windows, Brewfile on macOS.
- ADD `.github/workflows/ci.yml` — matrix on `windows-latest` + `macos-latest`. Runs configure → build → test on both. Phase 0 exit gate.
- ADD `vcpkg.json` (Windows) and a `Brewfile` or equivalent (macOS) for reproducible deps.

---

## c) Backend / Hosting Plan

The user said "not run locally." The TUI binary still runs on each operator's machine; the data, the Plaid integration, and the audit log live elsewhere.

Three concrete options:

### Option A — Self-hosted single VPS (RECOMMENDED)

A small server we run somewhere (DigitalOcean droplet / Hetzner / equivalent). C++ HTTP service backed by SQLCipher. Plaid integration runs server-side on a 15-minute cron, mirroring GREYLOCK's design. TUIs authenticate against the server, fetch data over HTTPS, render.

Pros:
- Server-side cron enables 24/7 Plaid sync without any TUI being open.
- Server-side queries make Discovery + Consolidation cheap (no round-tripping every transaction).
- Single source of truth — Tristan and Rory always see the same numbers.
- Server holds Plaid creds (one place to rotate).
- Familiar deployment story; same operating model as GREYLOCK was designed around.

Cons:
- We have to run a server (operational burden — patching, backups, monitoring).
- Server compromise reads everything (mitigated by SQLCipher at rest + access via auth tokens only).
- Costs a few dollars a month.

### Option B — Encrypted-blob-store backend (zero-knowledge)

Server is dumb storage (S3-compatible bucket fronted by a thin auth API). All data is encrypted client-side; server sees only ciphertext. Plaid integration runs client-side when the TUI is open.

Pros:
- True zero-knowledge — even if the server is fully compromised, attacker gets ciphertext.
- Solves the personal-tier siloing claim that the TS prototype got wrong.

Cons:
- No server-side queries — every Discovery / Consolidation pass round-trips every transaction to a client.
- Plaid sync only happens when a TUI is open (no 24/7 cron).
- Plaid creds duplicate across each user's TUI.
- Schema migrations are painful (every client decrypts/re-encrypts).
- 2x the implementation work of Option A.

### Option C — Self-hosted backend + per-row envelope encryption (hybrid)

Server holds SQLCipher (defense in depth) AND each sensitive row (access tokens, transaction descriptions) is independently encrypted under a key the client provides via a session-bound credential. Server can read account ids + balances + dates (needed for queries / cron) but not transaction descriptions or tokens without an active session.

Pros:
- Server-side cron works (server holds enough to fetch from Plaid and store ciphertext).
- Discovery / Consolidation work in two passes: server filters by date / amount; client decrypts descriptions.
- Token-broker pattern naturally falls out: server decrypts the access token only inside one Plaid call's lifetime, never persists plaintext.

Cons:
- Most complex of the three. Lots of careful key management.
- Schema is uglier (every encrypted column is bytes, not text).

### Recommendation: Option A.

Rationale: 2–3 trusted operators on a server we control. The threat model that makes Option B's zero-knowledge property valuable (operator does not trust the host) doesn't apply — Rory and Tristan ARE the host. Option A's server-compromise risk is mitigated by SQLCipher + audit log. Option C is over-engineering for the threat model.

If Rory wants the zero-knowledge property anyway (e.g. because the future N-th user might not be fully trusted), Option C is the right answer; Option B is too operationally painful to recommend.

`[NEEDS RORY DECISION]` — see §i Q1.

---

## d) Auth Plan

Two concrete options:

### Option A — Passphrase + TOTP (RECOMMENDED for v0.2)

Per-user passphrase (server-side argon2id hash) + TOTP second factor (RFC 6238). Login from the TUI prompts for both. Server returns a session token (signed, short-lived, refreshable). Session token cached in OS secret store (DPAPI on Windows, Keychain on macOS) so users don't re-auth every TUI launch.

Pros:
- Solvable from a TUI without a paired browser.
- TOTP gives a real second factor.
- Session token in OS secret store mirrors the v0.1 SecurityService pattern Tristan already wrote.
- Cross-platform from the start.

Cons:
- Passphrase has the usual passphrase risks. Argon2id mitigates offline-cracking; we still rely on operator hygiene.
- TOTP requires an authenticator app.

### Option B — WebAuthn passkey via paired browser ceremony

TUI launches `https://auth.tf-server.example/login?session_id=<random>` in the system browser; user completes a WebAuthn assertion; backend generates a session token and writes it back to the TUI via a local listener or a one-time pickup endpoint.

Pros:
- Phishing-resistant.
- Closer to the long-term ideal.
- Avoids passphrase risk entirely.

Cons:
- The paired-browser ceremony adds a moving part.
- WebAuthn `prf` extension support is uneven; we'd need it to bind a per-user KEK to the passkey (see §e).
- Browser pairing is awkward for a server-side cron context (no user present).

### Recommendation: A for v0.2, B as a v0.3 add-on.

Layer them: passphrase + TOTP ships in v0.2, passkey enrollment becomes available in v0.3 and the passphrase becomes a recovery factor. This way we ship something usable, and we don't throw away work — the session-token / OS-secret-store machinery doesn't change.

`[NEEDS RORY DECISION]` — see §i Q2.

---

## e) Crypto Plan

Recommendation locks unless §c or §d forces otherwise:

### Library: libsodium

Why not OpenSSL: bigger surface, more room to misuse, more setup per platform.
Why not mbedTLS: smaller community, less battle-tested for the specific primitives we need.
Why libsodium: single dep, modern primitives, defaults are correct, available via vcpkg + brew.

Primitives we'll use:
- AEAD: `crypto_secretbox_xchacha20poly1305` (XChaCha20-Poly1305). 192-bit nonces — random nonces are safe at scale.
- KDF for passphrases: `crypto_pwhash` (Argon2id). `OPSLIMIT_MODERATE` + `MEMLIMIT_MODERATE` defaults to start; tune in load test.
- KDF for sub-keys: `crypto_kdf` (HKDF-equivalent).
- Hash for audit chain: `crypto_generichash` (BLAKE2b) — switch to SHA-256 if Rory wants byte-compat with the GREYLOCK chain; otherwise BLAKE2b is faster and modern.
- Constant-time compare: `sodium_memcmp`.
- Buffer zeroize: `sodium_memzero`.

### Cross-platform secret store: `ISecretStore` abstraction

```cpp
class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual bool put(std::string_view key, std::span<const std::byte> value) = 0;
    virtual std::optional<std::vector<std::byte>> get(std::string_view key) = 0;
    virtual bool remove(std::string_view key) = 0;
};
```

Two implementations selected at CMake time:
- `DpapiSecretStore` (Windows) — lifts v0.1's `CryptProtectData` / `CryptUnprotectData` + registry storage. Almost no rewrite.
- `KeychainSecretStore` (macOS) — `SecItemAdd` / `SecItemCopyMatching` / `SecItemDelete` against `kSecClassGenericPassword`. Service name `com.terminalfinance.secrets`.

Both encrypt at rest using OS-bound keys (DPAPI: per-user; Keychain: per-Apple-ID-and-machine). No KEK we manage; the OS handles it. This intentionally avoids the AUDIT.md-F1 mistake of inventing a bespoke KEK derivation.

### Envelope encryption (server-side, per-row sensitive data)

Used for at-rest data in the backend (per Option A). Each row's sensitive bytes get sealed with `crypto_secretbox` under a row-class DEK, where the DEK is derived from a master key held in process memory. Master key sourced at server boot from a secret-management story (env var on the VPS, or a managed secret store like Hashicorp Vault — see §i Q3).

Plaintext format on disk: never. All blobs are `version (1 byte) || nonce (24 bytes) || ciphertext || tag (16 bytes)`.

### `withDecryptedToken<T>` pattern in C++

```cpp
template <typename F>
auto PlaidService::withDecryptedToken(std::string_view account_id, F&& f)
    -> std::invoke_result_t<F, std::string_view>;
```

Implementation outline:
1. Look up the encrypted token from the secret store (or backend, depending on §c) by `account_id`.
2. Decrypt into a `std::vector<std::byte>` whose destructor zeroes via `sodium_memzero`.
3. Construct a `std::string_view` over those bytes; pass to `f`.
4. Buffer destructs at scope exit; bytes are zeroed deterministically.
5. Return `f`'s result. The plaintext token never escapes the lambda.

The existing `IPlaidService::get_transactions(access_token, ...)` interface is **renamed** to a private `get_transactions_with_token` and the public method becomes `get_transactions(account_id, ...)` which calls the broker internally. Existing callers update from `service.get_transactions(token, ...)` to `service.get_transactions(account_id, ...)`.

### AUDIT.md guardrails baked in

- **F-1 (no KEK from public credentialId):** if §d picks Option B (passkey), the per-user KEK MUST come from the WebAuthn `prf` extension or an equivalent user-bound secret. Do not derive a KEK from `credential.id`.
- **F-2 (peer-cred check that doesn't check):** N/A in v0.2 (no IPC keybridge), but the principle applies — never write a security check whose implementation doesn't match its claim.
- **F-4 (timestamp double-counting):** see §f. Audit canonical bytes use `tsUnixNanos = ms_since_epoch * 1_000_000` exactly. No additional sub-second component.
- **F-5 (`x-forwarded-for` trusted):** rate-limiting on the backend keys on `(authenticated_user_id, endpoint)`, not on `X-Forwarded-For`. The server reads `X-Forwarded-For` only for logging context, never for auth or rate-limit decisions.

`[NEEDS RORY DECISION]` — see §i Q3 (master-key sourcing on the server).

---

## f) Audit Log Plan

Lives on the server (per Option A). Append-only, hash-chained, transaction-atomic.

### Canonical bytes (fixing AUDIT.md F-4)

Each entry hash covers, in order:

```
seq           : uint64 BE
tsUnixNanos   : int64 BE   = ms_since_epoch * 1_000_000   (NO SECOND TERM)
actorUserId   : utf8 || 0x00
actorKind     : utf8 || 0x00            (user | system | sync_worker)
domain        : utf8 || 0x00            (entity_id; "" if not entity-scoped)
subjectId     : utf8 || 0x00
subjectKind   : utf8 || 0x00
action        : utf8 || 0x00
outcome       : utf8 || 0x00
detailsJson   : len32be(bytes) || bytes
prevHash      : 32 bytes
entryHash     : BLAKE2b-256(canonical bytes above)
```

**Specifically NOT included**: a separate `tsNanos` sub-millisecond field. JS Date couldn't produce it; in C++ we'd have access to higher-resolution clocks but the chain doesn't need it for ordering (the `seq` field already provides total order). One time field, multiplied to nanos, no second component.

### Atomic append

SQLite transaction:
1. `SELECT entryHash FROM audit_log ORDER BY seq DESC LIMIT 1` (locks the chain head with `BEGIN IMMEDIATE`).
2. Compute `seq = head.seq + 1`, `prevHash = head.entryHash` (or 32 zero bytes for `seq = 1`).
3. Compute canonical bytes; hash; insert.
4. Commit.

Failed inserts (sanitizer-rejected payload, etc.) never reach the SELECT — they fail at the service boundary before transaction begin.

### Sanitizer (port from `lib/audit/sanitizer.ts`)

C++ implementation of the closed-by-default deny list:
- Allowed-keys set: `userId`, `sessionId`, `accountId`, `transactionId`, `entityId`, `tokenId`, `subjectId`, `actorUserId`, `domain`, `outcome`, `action`, `kind`, `reason`, `version`, `seq`, `ts`, `count`, `added`, `modified`, `removed`, `httpStatus`, `errorCode`, `transports`.
- Deny key substrings (case-insensitive): `password`, `passphrase`, `secret`, `token`, `cookie`, `dek`, `kek`, `keksalt`, `credentialpublickey`, `signature`, `pem`, `key` (last; carve-out via allow list).
- Token-shape value rejection: any string of length ≥ 32 matching `/^[A-Za-z0-9+/_-]+={0,2}$/` or `/^[0-9a-fA-F]{32,}$/`.
- Total payload cap: 64 KiB serialized.
- Depth cap: 8.

Reject = whole append rejected with `{kind: 'sanitizer_rejected_payload'}`. Don't strip-and-keep — we'll miss something.

### Replay-verification test BEFORE writer

Per AUDIT.md guidance ("write the replay-verification test before the writer"). The verifier walks rows in `seq`-ascending order, recomputes each `entryHash`, asserts byte-for-byte equality. Test fixtures cover: clean chain, tampered `detailsJson`, tampered `prevHash`, missing seq, duplicate seq, empty chain, single-row chain. Verifier ships first; writer ships after the verifier passes against hand-rolled chains.

### What gets logged

- Auth events: `passkey_authentication_success`, `passkey_authentication_failure` (or `passphrase_*` per §d), `session_created`, `session_revoked`, `session_expired`, `auth_rate_limit_tripped`.
- Plaid events: `plaid_link_token_minted`, `plaid_public_token_exchanged`, `plaid_item_added`, `plaid_item_removed`, `plaid_token_decrypted`, `plaid_sync_started`, `plaid_sync_completed`, `plaid_sync_failed`.
- Admin events: `admin_user_invite_minted`, `admin_user_revoke_invoked`, `admin_master_rotation_*`.
- Crypto events: `master_key_loaded`, `master_key_unloaded`, `per_row_dek_zeroized`.
- Discovery / Consolidation events at WARNING outcome only (no need to chain every per-tx classification).

---

## g) Phase Plan

Six phases. Phase 0 is non-negotiable; nothing else dispatches until it lands.

### Phase 0 — Cross-platform build + CI

**Lead:** build-engineer, http-client-engineer.
**Goal:** v0.1 source compiles on Windows AND macOS, identical executable behavior. Existing tests pass on both. CI matrix is green.
**Concrete tasks:**
- CMakeLists: platform-conditional library linking. `if(WIN32) ... target_link_libraries(... crypt32 winhttp advapi32) ... elseif(APPLE) ... target_link_libraries(... "-framework Security" "-framework CoreFoundation" curl) ...`.
- `SecurityService` becomes `ISecretStore` interface + `DpapiSecretStore` (Windows) + `KeychainSecretStore` (macOS) skeletons. Wire via CMake target source list selection.
- Replace `winhttp` usage with libcurl (the only network call in v0.1 is in `PlaidService.cpp`, which I haven't read but the include surface implies winhttp).
- Add `vcpkg.json` (Windows) and `Brewfile` (macOS) for reproducible deps.
- Add `.github/workflows/ci.yml` matrix: configure → build → test on both runners.
**Exit:** Green CI on both runners. Tristan (Windows) and Rory (macOS) each run `./TerminalFinance` from a fresh checkout and the existing four-tab dashboard renders.

### Phase 1 — Crypto + secret-store abstraction

**Lead:** crypto-engineer. Reviewer: security-reviewer.
**Goal:** `ISecretStore` is a real cross-platform abstraction; libsodium integrated; `withZeroized<T>` RAII helper landed.
**Tasks:**
- libsodium via vcpkg + Brewfile, FetchContent fallback.
- `DpapiSecretStore` and `KeychainSecretStore` complete implementations, behind one set of tests (same test runs against both).
- `withZeroized<T>` and constant-time compare helpers.
- Envelope-encryption helpers for the future per-row encryption.
**Exit:** All `ISecretStore` tests pass on both OSes. Round-trip property test: `put(k,v); get(k) == v` on a 10K-byte payload.

### Phase 2 — Backend skeleton + transport

**Lead:** backend-architect, supported by http-client-engineer.
**Goal:** A tiny C++ HTTP server that speaks JSON, an `IHttpClient` (libcurl) on the client side, and the wire protocol scaffolding for `BackendClient`.
**Tasks:**
- Server: pick a C++ HTTP server framework (Crow / Drogon / cpp-httplib server mode). RECOMMENDATION: `cpp-httplib` for simplicity unless backend-architect surfaces a reason against. Single dependency, header-only, runs on Windows + macOS + Linux.
- Schema bootstrap: SQLite + SQLCipher tables for `users`, `entities`, `entity_memberships`, `accounts`, `transactions`, `audit_log`, `sessions`, `enrollment_tokens`. Write the schema migrations from day one.
- Transport: HTTPS + TLS 1.3, JSON request/response, structured error codes.
- One round-trip endpoint working end-to-end: `GET /healthz` → `{ok: true}`, no auth.
**Exit:** TUI on Tristan's Windows box can `GET /healthz` against a server running on Rory's macOS box (and vice versa), over HTTPS, with cert pinning or trust-on-first-use (TOFU).

### Phase 3 — Auth

**Lead:** auth-engineer. Reviewer: security-reviewer.
**Goal:** End-to-end login/logout from TUI to backend.
**Tasks:**
- `users` table schema. Enrollment via admin invite token (one-shot, hashed, expiring — port `lib/auth/enrollment-token.ts` semantics).
- Passphrase hash via `argon2id` (libsodium `crypto_pwhash`). TOTP via `crypto_auth_hmacsha1` building blocks (or `cpp-jwt` style; pick at dispatch time).
- Session table. Sliding window + absolute timeout (30 min idle / 8 h absolute, mirroring TS).
- TUI login flow: prompt passphrase + TOTP, send to `POST /auth/login`, receive session token, store in `ISecretStore` keyed by `tf-session-<user_email>`.
- `AuthService::current_user()` for the TUI to gate views.
- Audit log entries on every auth event.
**Exit:** Tristan runs `./TerminalFinance --login`, types passphrase + TOTP, gets a session, exits, relaunches, and `current_user()` returns him without re-prompting. Logout clears the session.

### Phase 4 — Storage migration + Plaid token broker + audit log

**Leads:** backend-architect, plaid-engineer, audit-log-engineer (parallel where possible). Reviewer: security-reviewer for plaid + audit-log.
**Goal:** All data lives off-machine. Plaid tokens never plaintext outside one broker scope. Audit log writes on every sensitive op.
**Tasks:**
- Server: `entities`, `accounts`, `transactions`, `categories`, `budgets` endpoints. CRUD + bulk-fetch. JSON over HTTPS. Sensitive columns (token blob; transaction descriptions if §c picks Option C) are byte columns with envelope encryption.
- Server: `audit_log` writer per §f. Replay-verification test ships first, writer ships against it.
- Server: Plaid integration server-side. Cron-like scheduler (15-min interval). Token-broker pattern at the server boundary.
- Client: `RemoteBackendStorageService` implementing `IStorageService`. Client-side view of `DataStore` becomes a cache; mutations go through `BackendClient`.
- Client: `PlaidService::withDecryptedToken<T>` template. Refactor `Account.plaid_access_token` removal. Add `is_plaid_linked` bool.
- Migration: one-time tool that reads an existing v0.1 JSON file and uploads its contents through the new endpoints. Audit-emit `migrated_from_local`.
**Exit:** A full Plaid sync runs server-side, writes encrypted blobs to SQLCipher, audit log entries chain correctly, TUIs see updated transactions on next refresh, no plaintext token exists in any process for longer than one Plaid SDK call.

### Phase 5 — Wire `proposals/` + Discovery + Consolidation polish

**Leads:** tui-engineer, discovery-engineer, consolidation-engineer.
**Goal:** Tristan's drafted FTXUI components are live in the binary. Shovel Intelligence is real, not stubbed.
**Tasks:**
- tui-engineer inventories `proposals/` against current state, promotes ready ones, files issues for the rest.
- discovery-engineer: `SupplierMapping` becomes JSON-loadable; backend serves the canonical list at `GET /supplier-map`. MoM velocity gets unit tests against realistic merchant strings (StarbucksUS123, AMZN MKTPLACE * SUBSCRIBE, COSTCO WHSE #0123, USPS PO 12345, etc.).
- consolidation-engineer: cross-account-same-transaction case test. Idempotency test (running a sync twice produces the same DataStore).
- TUI snapshot harness lands here so the new views can be regression-tested.
**Exit:** Dashboard shows live `ui_net_worth`, `ui_category_trends`, `ui_shovel_intelligence`, `ui_shovel_score`, `ui_sync_status` panels. Snapshot tests pass on both OSes.

### Phase 6 — Test parity + docs

**Leads:** test-engineer, docs-engineer.
**Goal:** Coverage parity per service; docs for v0.2.
**Tasks:**
- GoogleTest coverage report integrated into CI. Set a floor (e.g. 70% line coverage on new code, 50% project-wide).
- TUI snapshot harness deterministic across OSes. Deal with terminal width / locale / line-ending differences.
- README rewrite for the v0.2 architecture.
- New `docs/ARCHITECTURE.md`, `docs/THREAT_MODEL.md` (off-machine model), `docs/RUNBOOK.md` (server ops), `docs/CONTRIBUTING.md` (per-platform build).
- Migration guide: how to move from v0.1 (local JSON) to v0.2 (off-machine).
**Exit:** CI green with coverage gates. Docs reviewed by Rory. v0.2 tag.

---

## h) Test Strategy

### Per-service GoogleTest coverage map

- `Validator` — KEEP coverage; it already gates DataStore writes.
- `DataStore` — extend `test_datastore.cpp` to ~30 cases. Cover: entity-membership filtering, budget rollups, transaction filters, snapshot computation.
- `SecurityService` (now `ISecretStore`) — round-trip across both implementations, against a 10KB payload, against a unicode payload, against the "missing key" path, against the "wrong-key reject" path.
- `PlaidService` — broker-pattern test: assert plaintext token does not exist in process memory after `withDecryptedToken` returns (use `sodium_memzero` checks). Stub-mode tests for all interface methods. Real-mode tests gated behind `PLAID_INTEGRATION_TESTS=1`.
- `DiscoveryService` — realistic merchant strings; MoM velocity correctness; `refresh_supplier_data` from a fixture file.
- `ConsolidationService` — dedup idempotency, cross-account-same-transaction, hash-stability across runs.
- `AuthService` — session lifecycle, expiry, sliding window, rate-limit on failed logins, TOTP correctness.
- `AuditLogClient` + server-side writer — replay verification (SHIPS FIRST), then chain construction, then sanitizer rejection cases.
- `IHttpClient` — TLS verification, timeout, redirect handling, retry behavior.
- `BackendClient` — error mapping, session-token attachment, retry on 401 → re-login flow.

### TUI snapshot harness

- Each view renders via `ftxui::Render(view, screen)` with a fixed-size `Screen(120, 40)`.
- Output canonicalized: line-by-line trimmed-trailing-whitespace, normalized line-endings.
- Fixture data via `SyntheticGenerator` so views don't depend on Plaid.
- Goldens checked in under `tests/snapshots/*.txt`. CI fails on diff. Updates require explicit `--update-snapshots` flag during dev.

### Determinism requirements

- No real-time clocks in golden paths; inject `std::chrono::system_clock` mockable via `IClock`.
- No real RNG for tests; fixed-seed `std::mt19937` injected.
- No real network; all backend calls go through a fake `IHttpClient` in unit tests, real `cpp-httplib` test server in integration tests.
- No real filesystem; in-memory storage where possible.

### AUDIT.md-driven adversarial tests

For each AUDIT.md finding, an explicit regression test guarantees it doesn't reappear:
- `test_audit_chain_canonical_bytes_no_double_count` — assert the canonical-bytes function produces the right byte sequence for a hand-rolled fixture, and the byte length matches expectation.
- `test_no_kek_from_credential_id` — type-system test (compile-time) that the user-KEK derivation function does not take a `credential_id`-typed input.
- `test_rate_limit_keys_on_user_id_not_xff` — a request with a forged `X-Forwarded-For` header still hits the same rate-limit bucket.

---

## i) Open Questions for Rory

These all gate Phase 1+ dispatch. The orchestrator should not start any phase whose tasks depend on an unresolved Q below.

### Q1 — Backend shape (gates §c, Phase 2)

A. Self-hosted single VPS, server-side queries, server-side Plaid cron. (RECOMMENDED — §c)
B. Encrypted-blob-store backend, client-side everything, TUI-open-only sync.
C. Hybrid (per-row envelope encryption with session-bound DEKs).

**Pick one.** A is simpler and covers the threat model. B is more secure if you don't trust the host; you ARE the host. C is over-engineering unless future N-th user is untrusted.

### Q2 — Auth mechanism (gates §d, Phase 3)

A. Passphrase + TOTP for v0.2; passkey enrollment in v0.3 layered on top. (RECOMMENDED — §d)
B. WebAuthn passkey via paired-browser ceremony, day one.
C. Passphrase only (no second factor).

**Pick one.** A ships fastest with real 2FA. B is phishing-resistant but adds moving parts. C is below the security bar for financial data.

### Q3 — Server master-key sourcing (gates Phase 4)

The backend needs a master encryption key in process memory at boot. Where does it come from?

A. Env var on the VPS, manually rotated. Simple, no third-party dep. (RECOMMENDED for v0.2)
B. Hashicorp Vault / AWS KMS / equivalent. Operationally heavier; better key-rotation story.
C. SSH-passphrase prompt on server boot — operator types a passphrase to bring the server up, master KEK derives via argon2id. Loses unattended restarts.

**Pick one.** A for v0.2; we can migrate to B in v0.3 if the threat model demands.

### Q4 — Audit-log hash function (gates §f)

A. BLAKE2b-256 (libsodium native, faster). (RECOMMENDED)
B. SHA-256 (byte-compatible with the GREYLOCK chain construction; matters only if you want to verify both chains with the same tool).

**Pick one.** A unless you specifically want byte-compat with the TS prototype.

### Q5 — Server hosting (gates Phase 2 deploy)

Where does the VPS live? DigitalOcean / Hetzner / Linode / Tristan's homelab / Rory's homelab / something else?

This affects: latency to Plaid's US-East endpoints, cert provisioning, backup story, who has root on the box.

**Pick a vendor.** Suggestion: Hetzner CX21 in Ashburn, VA — cheap (~$5/mo), low latency to Plaid, EU operator (privacy-friendlier than US-based clouds for non-customer-data).

### Q6 — Cert / TLS strategy (gates Phase 2 transport)

A. Let's Encrypt against a real domain (`tf.terminalfinance.<your-domain>.com`).
B. Self-signed cert pinned in the TUI binary at build time.
C. mkcert-style local CA installed on each operator's machine.

**Pick one.** A is simplest if you have a domain. B is most localhost-feeling. C requires per-machine CA install.

### Q7 — Dev / staging backend instance

Do we want a separate staging instance for development, or do Tristan and I just point at a local-machine backend during dev and the production VPS for real?

A. Single production instance; dev runs against local-machine backend.
B. Production + staging on the same VPS, different ports / databases.
C. Production VPS + dev `docker compose` recipe per developer.

**Pick one.** A is cheapest. C is most professional. B is an ugly compromise.

### Q8 — User onboarding flow

How does a new user (e.g. eventually Cade) get onboarded?

A. Rory mints an enrollment token via admin CLI on the server; emails it to the new user; new user runs `./TerminalFinance --enroll <token>`. Mirrors GREYLOCK's flow.
B. Rory creates the user record server-side with a temp passphrase; new user logs in and is force-rotated on first login.
C. Self-serve registration with email verification (overkill for 3 users).

**Pick one.** A is the ergonomic match for the platform. B is simpler to implement.

### Q9 — Demo / synthetic data

The current `SyntheticGenerator` produces fake data for dev. Is the goal a full "demo mode" that's selectable from the TUI, or is synthetic data only for tests?

A. Synthetic only in tests. Production runs against real Plaid.
B. Demo mode toggleable from `.env` (`TF_DEMO_MODE=1`); shows realistic-looking but fake data so you can pitch the platform without exposing real numbers.
C. Both: tests use synthetic; an explicit `--demo` flag on the binary launches a separate demo-data instance.

**Pick one.** B is useful if Tristan ever shows this to anyone. A is simplest.

### Q10 — License / repo ownership

Currently the repo is `tristanmloftus/GREYLOCK`. As the project name shifts to TerminalFinance, do we want to:

A. Rename the repo to `tristanmloftus/TerminalFinance`. Easy for GitHub; old links 301-redirect.
B. Move to a shared org (e.g. `loftus-bros/TerminalFinance`). Cleaner for collaboration; requires creating an org.
C. Leave the repo named GREYLOCK and just rename the project internally. Confusing forever.

**Pick one.** B is the long-term answer if this becomes anything. A is fine for now.

---

## Stop conditions

The orchestrator should stop dispatching and surface any of these to Rory:

1. Any of Q1–Q10 above is unresolved when its dependent phase comes up.
2. A subagent is rejected 3 times in a row on the same task.
3. A subagent's output reveals the plan is wrong (e.g. an architectural assumption doesn't compile, a library doesn't exist on a target platform).
4. CI cannot be made green on both OSes after a phase.
5. A security-reviewer subagent issues a `REJECT` that cannot be addressed within the current phase.

End of plan.
