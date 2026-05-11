# Architecture

TerminalFinance v0.2 is a two-process system: a single-binary FTXUI client
(`TerminalFinance`) and a single-binary HTTPS server (`TerminalFinanceServer`).
They communicate over JSON-over-HTTPS with bearer-token sessions. All
persistent state lives in the server's SQLCipher database.

This document walks the boundary between the two, the modules inside each,
the on-disk schema, and the cross-cutting concerns (audit log, Plaid token
broker, sessions) that the code centralizes by design. Crypto guarantees are
expanded in [THREAT_MODEL.md](THREAT_MODEL.md); operator-side controls in
[RUNBOOK.md](RUNBOOK.md).

## System overview

Two binaries, talking only over HTTPS:

```
+------------------------+                            +------------------------+
| TerminalFinance        |                            | TerminalFinanceServer  |
| (TUI client)           |                            | (HTTPS server)         |
|                        |  POST /auth/login          |                        |
|  src/main.cpp          | -------------------------> |  server/main.cpp       |
|  src/views/*           |    { email, passphrase,    |  server/http/Server.*  |
|  src/services/*        |      totp_code }           |                        |
|                        |                            |                        |
|                        | <------------------------- |  AuthHandlers          |
|                        |  { session_token, ... }    |                        |
|                        |                            |                        |
|                        |  GET /entities             |                        |
|                        |  Authorization: Bearer ... | -> SessionMiddleware   |
|                        |                            | -> EntitiesHandler     |
|                        | <------------------------- |    + audit_log.record  |
+------------------------+                            +-----------+------------+
                                                                  |
                                                                  v
                                                      +-----------------------+
                                                      |  SQLCipher data.db    |
                                                      |  (server/db/...)      |
                                                      +-----------+-----------+
                                                                  |
                                                                  v
                                                      +-----------------------+
                                                      |  PlaidSyncScheduler   |
                                                      |  -> PlaidApiClient    |
                                                      |  -> Plaid HTTPS API   |
                                                      |  (15-min cron)        |
                                                      +-----------------------+
```

Cert pinning is via `CurlHttpClient` (HTTPS-only protocol allowlist on both
initial request and redirects, `src/services/http/CurlHttpClient.cpp:165-166`)
and the system trust store. Dev TLS certs come from mkcert
(`scripts/generate-dev-cert.sh`).

### Request lifecycle: login → CRUD

```
1. User runs:  ./TerminalFinance --login
   src/main.cpp:418 (cmd_login)
   -> prompts passphrase (echo off, src/main.cpp:282)
   -> prompts TOTP code

2. AuthService::login() (src/services/AuthService.cpp)
   -> BackendClient.post("/auth/login", { email, passphrase, totp_code })

3. CurlHttpClient sends HTTPS POST
   -> server enforces TLS via cpp-httplib SSLServer
   -> AuthHandlers (server/auth/AuthHandlers.cpp:626) handles route

4. Server side:
   a. rate_limit_check("auth_login:" + email, now_unix)   <- F-5
   b. verify_passphrase() via Argon2id                    <- C-1 dummy hash on miss
   c. verify_totp()                                       <- constant-time
   d. mint_session() (server/auth/Session.cpp:72)
      -> 32 random bytes -> base64url for client
                          -> BLAKE2b-256 -> hex id for sessions table
   e. audit_log.record("auth_login", outcome="ok")

5. Server returns { session_token, user_id, expires_at_unix }.

6. AuthService caches session_token via ISecretStore
   key = "tf-session-" + email
   (DPAPI on Windows, Keychain on macOS; src/services/secret_store/)

7. Later: ./TerminalFinance (default — launches TUI)
   -> src/main.cpp:703-722 checks current_user_id()
   -> AuthService.current_user_id() reads cached token, GET /auth/whoami
   -> if 401, cache cleared and user is told to re-login

8. Data ops route through BackendClient with session header:
   GET /entities/.../accounts -> SessionMiddleware::require_session ->
     AccountsHandler -> SqlAuditLog.record(...) -> JSON response
```

## Module map

### `src/` — TUI client

| Path | Role | Entry point |
|------|------|-------------|
| `src/main.cpp` | Process entry; CLI flag dispatch; FTXUI render loop | `main()` |
| `src/models/DataStore.cpp` | In-memory cache of entities/accounts/transactions | `class DataStore` |
| `src/models/{Entity,Account,Transaction,Category,Budget}.h` | Plain-data types | n/a |
| `src/services/ServiceContainer.h` | DI for the services below | `class ServiceContainer` |
| `src/services/BackendClient.cpp` | HTTPS JSON client; structured `BackendError` mapping | `BackendClient::post/get` |
| `src/services/AuthService.cpp` | login/logout/enroll/whoami + secret-store session cache | `AuthService::login` |
| `src/services/IHttpClient.h` + `http/CurlHttpClient.cpp` | libcurl wrapper, HTTPS-only, CRLF-guarded | `CurlHttpClient::send` |
| `src/services/ISecretStore.h` + `secret_store/` | DPAPI / Keychain abstraction (no Linux impl) | `put`/`get`/`remove` |
| `src/services/PlaidService.cpp` | Account-id-keyed Plaid API (no plaintext tokens) | `IPlaidService::link_account` |
| `src/services/RemoteBackendStorageService.cpp` | `IStorageService` over `BackendClient` | `RemoteBackendStorageService::load` |
| `src/services/StorageService.cpp` | Legacy local `JsonStorageService` (kept for migration) | `JsonStorageService::load` |
| `src/services/DiscoveryService.cpp` | Supplier mapping + MoM velocity (Shovel) | `DiscoveryService::map_to_supplier` |
| `src/services/ConsolidationService.cpp` | Cross-account dedup + total liquidity | `ConsolidationService::dedup` |
| `src/services/crypto/EnvelopeEncryption.cpp` | XChaCha20-Poly1305 IETF + ZeroizingBuffer + ConstantTime | `encrypt`/`decrypt` |
| `src/views/DashboardView.cpp` | Composes 5 widgets into the live dashboard | `DashboardView::render` |
| `src/views/widgets/*` | Seven FTXUI widgets promoted from `proposals/` in Phase 5 | `*Renderer(...)` functions |
| `src/migration/V01Migrator.cpp` | One-shot v0.1 JSON → server CRUD migration tool | `V01Migrator::migrate` |
| `src/utils/Logger.h`, `ConfigManager.cpp`, `Validator.cpp`, `SyntheticGenerator.cpp` | Cross-platform utilities | n/a |

### `server/` — HTTPS server

| Path | Role | Entry point |
|------|------|-------------|
| `server/main.cpp` | Boot, env policy (`TF_MASTER_KEY` enforcement), route wiring | `main()` |
| `server/http/Server.cpp` | cpp-httplib `SSLServer` wrapper | `Server::start` |
| `server/http/HealthzHandler.cpp` | `GET /healthz` → `{"ok":true,"version":"0.2"}` | `register_healthz_handler` |
| `server/db/Database.cpp` | SQLCipher-aware SQLite open + `prepare`/`exec` helpers | `class Database` |
| `server/db/Migrations.cpp` | M001 initial schema, M002 categories, M003 budgets, M004 plaid sync state | `apply_pending` |
| `server/auth/PassphraseHash.cpp` | Argon2id (`crypto_pwhash_str` MODERATE) | `hash_passphrase`, `verify_passphrase` |
| `server/auth/Totp.cpp` | RFC 6238 TOTP with `crypto_auth_hmacsha1` | `verify_totp` |
| `server/auth/EnrollmentToken.cpp` | One-shot hashed invite tokens | `mint_enrollment_token`, `persist_enrollment_token` |
| `server/auth/Session.cpp` | 32-byte random token, BLAKE2b-256 stored, sliding window | `mint_session`, `validate_and_touch_session`, `revoke_session` |
| `server/auth/AuthHandlers.cpp` | `POST /auth/{enroll,login,logout}`, `GET /auth/whoami` + rate limit | `register_auth_handlers` |
| `server/auth/SessionMiddleware.cpp` | `require_session` helper used by every data endpoint | `require_session` |
| `server/audit/AuditEvent.h` + `IAuditLog.h` | Audit event type + interface | `IAuditLog::record` |
| `server/audit/CanonicalBytes.cpp` | Canonical-bytes encoder + BLAKE2b-256 entry hash | `compute_canonical_bytes`, `compute_entry_hash` |
| `server/audit/Sanitizer.cpp` | Closed-by-default key allow-list + token-shape value rejection | `sanitize` |
| `server/audit/SqlAuditLog.cpp` | Transactional append + replay verifier | `SqlAuditLog::record`, `verify_chain` |
| `server/audit/StubAuditLog.cpp` | Stderr-only fallback (used in tests + dev) | `StubAuditLog::record` |
| `server/data/EntitiesHandler.cpp` | `/entities` CRUD | `register_entities_handlers` |
| `server/data/AccountsHandler.cpp` | `/entities/:id/accounts` and `/accounts/:id` CRUD | `register_accounts_handlers` |
| `server/data/TransactionsHandler.cpp` | `/accounts/:id/transactions` and `/transactions/:id` CRUD | `register_transactions_handlers` |
| `server/data/CategoriesHandler.cpp` | `/entities/:id/categories` and `/categories/:id` CRUD | `register_categories_handlers` |
| `server/data/BudgetsHandler.cpp` | `/entities/:id/budgets` and `/budgets/:id` CRUD | `register_budgets_handlers` |
| `server/data/EntityMembership.cpp` | `user_id` × `entity_id` authorization helper | `is_member`, `assert_member_or_403` |
| `server/plaid/PlaidTokenBroker.cpp` | Encrypted-token store + `withDecryptedToken<F>` scope | `PlaidTokenBroker::withDecryptedToken` |
| `server/plaid/PlaidApiClient.cpp` | libcurl-backed Plaid API client | `PlaidApiClient::transactions_sync` |
| `server/plaid/PlaidSyncScheduler.cpp` | 15-minute cron over linked accounts | `PlaidSyncScheduler::start/stop` |
| `server/discovery/SupplierMapHandler.cpp` | `GET /supplier-map` (session-gated; reads `data/supplier_map.json`) | `register_supplier_map_handler` |

## Data model

The SQLCipher database is built from four migrations in
`server/db/Migrations.cpp`. Table-by-table:

### `users` (M001, `Migrations.cpp:189-197`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | UUID-like hex from `randombytes_buf` |
| `email` | TEXT NOT NULL UNIQUE | |
| `created_at_unix` | INTEGER NOT NULL | |
| `passphrase_hash` | BLOB NOT NULL | Argon2id-encoded string bytes |
| `totp_secret` | BLOB | Per-user TOTP secret |

### `entities` (M001, `Migrations.cpp:203-212`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | |
| `name` | TEXT NOT NULL | |
| `kind` | TEXT NOT NULL | CHECK in (`Individual`, `LLC`, `Corporation`, `Partnership`, `Trust`, `Other`) |
| `created_at_unix` | INTEGER NOT NULL | |

### `entity_memberships` (M001, `Migrations.cpp:218-227`)

| Column | Type | Notes |
|--------|------|-------|
| `user_id` | TEXT NOT NULL | FK users(id) |
| `entity_id` | TEXT NOT NULL | FK entities(id) |
| `role` | TEXT NOT NULL | |
| | | PRIMARY KEY (`user_id`, `entity_id`) |

### `accounts` (M001, `Migrations.cpp:234-248`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | |
| `entity_id` | TEXT NOT NULL | FK entities(id) |
| `name`, `kind` | TEXT NOT NULL | |
| `balance_cents` | INTEGER NOT NULL DEFAULT 0 | |
| `plaid_item_id`, `plaid_account_id` | TEXT | |
| `encrypted_token` | BLOB | Envelope-encrypted via PlaidTokenBroker |
| `is_plaid_linked` | INTEGER NOT NULL DEFAULT 0 | |
| `created_at_unix` | INTEGER NOT NULL | |

### `transactions` (M001, `Migrations.cpp:258-274`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | |
| `account_id` | TEXT NOT NULL | FK accounts(id) |
| `plaid_transaction_id` | TEXT | |
| `posted_at_unix` | INTEGER NOT NULL | |
| `amount_cents` | INTEGER NOT NULL | |
| `description_encrypted` | BLOB | |
| `category` | TEXT | |
| `created_at_unix` | INTEGER NOT NULL | |
| | | INDEX `idx_transactions_account_posted` on (`account_id`, `posted_at_unix`) |

### `audit_log` (M001, `Migrations.cpp:284-299`)

| Column | Type | Notes |
|--------|------|-------|
| `seq` | INTEGER PK AUTOINCREMENT | gaps never reused (chain integrity) |
| `ts_unix_nanos` | INTEGER NOT NULL | `ms_since_epoch * 1_000_000` (F-4) |
| `actor_user_id` | TEXT | |
| `actor_kind` | TEXT NOT NULL | `user` / `system` / `sync_worker` |
| `domain` | TEXT | entity_id, or empty |
| `subject_id`, `subject_kind` | TEXT | |
| `action`, `outcome` | TEXT NOT NULL | |
| `details_json` | BLOB | sanitized JSON |
| `prev_hash` | BLOB NOT NULL | 32 bytes |
| `entry_hash` | BLOB NOT NULL | 32 bytes (BLAKE2b-256) |

### `sessions` (M001, `Migrations.cpp:306-314`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | hex of BLAKE2b-256(raw_token_bytes) |
| `user_id` | TEXT NOT NULL | FK users(id) |
| `created_at_unix`, `last_seen_unix`, `expires_at_unix` | INTEGER NOT NULL | sliding window enforced in code |
| `revoked` | INTEGER NOT NULL DEFAULT 0 | |

### `enrollment_tokens` (M001, `Migrations.cpp:322-330`)

| Column | Type | Notes |
|--------|------|-------|
| `token_hash` | BLOB PK | hashed one-shot enrollment token |
| `email` | TEXT NOT NULL | |
| `created_at_unix`, `expires_at_unix` | INTEGER NOT NULL | |
| `consumed_at_unix` | INTEGER | NULL until redeemed |

### `categories` (M002, `Migrations.cpp:118-128`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | |
| `entity_id` | TEXT NOT NULL | FK entities(id) |
| `name`, `kind` | TEXT NOT NULL | |

### `budgets` (M003, `Migrations.cpp:133-145`)

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PK | |
| `entity_id` | TEXT NOT NULL | FK entities(id) |
| `category_id` | TEXT | |
| `amount_cents` | INTEGER NOT NULL DEFAULT 0 | |
| `period_start_unix`, `period_end_unix` | INTEGER NOT NULL | |

### `plaid_sync_state` (M004, `Migrations.cpp:154-161`)

| Column | Type | Notes |
|--------|------|-------|
| `account_id` | TEXT PK | one row per linked account |
| `cursor` | TEXT NOT NULL DEFAULT `''` | Plaid `/transactions/sync` cursor |
| `last_sync_unix` | INTEGER NOT NULL DEFAULT 0 | |

`schema_migrations` is created by the migration runner before the first
migration applies (`Migrations.cpp:14-22`).

## Cross-platform abstractions

### `ISecretStore` — OS-backed secret storage

Interface in `src/services/ISecretStore.h`. Implementations:

| Platform | Source file | Backing store |
|----------|-------------|---------------|
| Windows  | `src/services/secret_store/DpapiSecretStore.cpp` | DPAPI (`CryptProtectData` / `CryptUnprotectData`) + HKCU registry (`Software\TerminalFinance\Tokens`) |
| macOS    | `src/services/secret_store/KeychainSecretStore.cpp` | `SecItemAdd`/`SecItemCopyMatching`/`SecItemDelete` under `kSecClassGenericPassword` |
| Linux    | (none)            | `CMakeLists.txt:282-288` sets `SECRET_STORE_SOURCES=""` and emits a build-time warning. `SecretStoreTests` is conditional on `WIN32 OR APPLE` (`CMakeLists.txt:443`). |

`main.cpp:119-123` selects the implementation at compile time. On Linux,
`core.secrets` is left null and `--enroll`/`--login`/`--logout`/`--whoami`
return an error if invoked (`src/main.cpp:633-637` etc.).

### `IHttpClient` — single libcurl-backed implementation

Interface in `src/services/IHttpClient.h`; sole impl in
`src/services/http/CurlHttpClient.cpp`. Hard constraints baked into the
implementation:

- **HTTPS-only protocol allowlist** for the initial URL and for redirect
  targets (`CurlHttpClient.cpp:165-166`).
- **CRLF rejection** in caller-supplied header names and values
  (`CurlHttpClient.cpp:47-50,181-189`) — defends against HTTP header
  injection / request smuggling.
- **TLS verification on** with the host's CA trust store; tests pin to
  `tests/fixtures/test-ca.pem` for the mkcert-issued dev cert.

`BackendClient` further refuses to construct with a non-HTTPS `base_url`
(`src/services/BackendClient.cpp:16-19`).

## Audit log

The audit log is the load-bearing piece of v0.2's security story. Layout:

1. **Canonical bytes** — `server/audit/CanonicalBytes.cpp:54-94` builds the
   pre-hash byte sequence:
   - `seq` (uint64 BE)
   - `tsUnixNanos` (int64 BE) = `ts_ms * 1_000_000`
     **exactly once** — the F-4 fix
   - NUL-terminated UTF-8 strings: `actor_user_id`, `actor_kind`, `domain`,
     `subject_id`, `subject_kind`, `action`, `outcome`
   - `details_json` as `len32be(bytes) || bytes`
   - `prev_hash` (32 bytes; throws if not 32, `CanonicalBytes.cpp:55-59`)
2. **Entry hash** — BLAKE2b-256 over the canonical bytes
   (`CanonicalBytes.cpp:100-118`, `crypto_generichash` with key=nullptr).
3. **Append** — `SqlAuditLog::record` wraps the operation in
   `BEGIN IMMEDIATE` so the chain head can be read and the new row inserted
   atomically. Failed sanitizer payloads are rejected before the
   transaction begins.
4. **Replay verifier ships first** — `tests/test_audit_replay.cpp` walks the
   chain row-by-row, recomputes each `entry_hash`, asserts byte equality,
   and exercises tampered-row / duplicate-seq / missing-seq fixtures.
   `AuditReplayTests` is wired in `CMakeLists.txt:1103-1121`. The writer
   (`server/audit/SqlAuditLog.cpp`) is regression-tested by
   `AuditWriterTests` (`CMakeLists.txt:1150-1168`) against the verifier.

The sanitizer (`server/audit/Sanitizer.cpp`) enforces a closed-by-default
key allow-list, deny substrings (`password`, `passphrase`, `secret`,
`token`, `cookie`, `dek`, `kek`, `keksalt`, `credentialpublickey`,
`signature`, `pem`, `key`), token-shape value rejection (length ≥ 32 + base64
or hex pattern), depth cap 8, and total payload cap 64 KiB. Rejection is
all-or-nothing — no strip-and-keep.

## Plaid token broker

Plaid access tokens are the second load-bearing secret. The broker
(`server/plaid/PlaidTokenBroker.{h,cpp}`):

1. Constructs either from a 32-byte master key span (preferred — used by
   `server/main.cpp:404-405` once `TF_MASTER_KEY` is parsed) or by reading
   `TF_MASTER_KEY` itself (`PlaidTokenBroker.cpp:87-141`).
2. Copies the master key into `master_key_`, derives a single DEK via
   `crypto_kdf_derive_from_key` with context `"tf-plaid"` and subkey id 1
   (`PlaidTokenBroker.cpp:32-55`), then **immediately zeroes**
   `master_key_` (`PlaidTokenBroker.cpp:74`). Only the DEK survives for the
   broker's lifetime — minimizing the master-key exposure window.
3. `store_token(account_id, plaintext)` calls
   `tf::crypto::encrypt(plaintext, aad=account_id_bytes, dek_)` and
   `UPDATE accounts SET encrypted_token=blob, is_plaid_linked=1 WHERE id=?`
   (`PlaidTokenBroker.cpp:158-190`). The blob is scrubbed before
   destructing.
4. `withDecryptedToken<F>(account_id, f)` fetches the blob, decrypts into a
   `ZeroizingBuffer`, hands `f` a `std::string_view` over those bytes, and
   on return zeroes the buffer (`PlaidTokenBroker.h:204-240`).
   **Plaintext token never escapes `f`'s scope.**
5. If no token is stored for `account_id`, the broker calls `f(NoTokenTag{})`
   — it never hands callers an empty string view claiming to be a token
   (F-2 honesty). The single-callable variant requires the lambda to be
   invocable with `NoTokenTag` as a compile-time check
   (`PlaidTokenBroker.h:159-163`).

The AEAD primitive is `crypto_aead_xchacha20poly1305_ietf` (192-bit nonce,
16-byte Poly1305 tag, AAD-bound) — see `EnvelopeEncryption.h:5-23`. On-disk
blob layout is `version(1) || nonce(24) || ciphertext+tag`.

## Session lifecycle

Defined in `server/auth/Session.{h,cpp}`:

- **Mint** (`Session.cpp:72-121`): `randombytes_buf(32)`. The
  base64url-encoded raw token goes to the client; the BLAKE2b-256 hash
  (hex-encoded) goes into `sessions.id`. The plaintext is never persisted
  or logged.
- **Validate + touch** (`Session.cpp:126-198`): wraps the read-check-update
  in `BEGIN IMMEDIATE`. Rejects revoked, expired
  (`expires_at_unix <= now_unix`), or idle
  (`last_seen_unix <= now_unix - 30*60`) sessions; otherwise updates
  `last_seen_unix` and returns `user_id`.
- **Revoke** (`Session.cpp:203-224`): `UPDATE sessions SET revoked=1 WHERE id=?`.

Constants in `server/auth/Session.h:35-36`:

| Constant | Value |
|----------|-------|
| `kSessionIdleTimeoutSeconds` | `30 * 60` (30 minutes) |
| `kSessionAbsoluteTimeoutSeconds` | `8 * 3600` (8 hours) |

Rate limit (`server/auth/AuthRateLimitInternal.h:21-23`,
`AuthHandlers.cpp:92-178`): bucket key is `"auth_login:" + email`
(`AuthHandlers.cpp:402-403`) — never the client IP or `X-Forwarded-For`
(F-5). Limit: 5 attempts per 15-minute window. The in-process map is
bounded at `kMaxRateBuckets = 10000`; saturation fails closed and emits a
single denial line.

## What is NOT included in v0.2

Explicit deferrals from `V0_2_PLAN.md`. None of the items below are
implemented; planning them is in scope for v0.3 or later.

- **WebAuthn / passkeys.** v0.2 ships passphrase + TOTP only
  (`V0_2_PLAN.md` §d, recommendation A). Passkey enrollment is layered on
  in v0.3 with passphrase becoming a recovery factor.
- **HSM- or KMS-backed master key.** `TF_MASTER_KEY` is sourced from an
  env var (`V0_2_PLAN.md` §i Q3, choice A). Vault / KMS integration is
  v0.3+.
- **Master-key rotation.** No support in v0.2 (`V0_2_PLAN.md` §i Q3).
- **Multi-user RBAC.** `entity_memberships` stores a `role` string but
  there are 2–3 trusted operators; no role-based authorization beyond
  membership exists yet.
- **Per-row envelope encryption for `transactions.description_encrypted`.**
  The column is declared (`Migrations.cpp:265`) but Phase 4 chose
  Option A (server-side queries) over Option C (per-row envelopes); the
  description field is currently written by `TransactionsHandler` as a
  blob without crypto wrapping at the handler layer. Re-encrypted-at-rest
  semantics rely on SQLCipher only.
- **Hosted CI on Windows / macOS.** Phase 0 originally planned a
  `windows-latest` + `macos-latest` matrix; `ci.yml` is currently
  single-job, self-hosted Linux (`.github/workflows/ci.yml:19-22`). Hosted
  runners may return in v0.3.
- **Demo mode.** `SyntheticGenerator` exists but is wired only for tests
  (`V0_2_PLAN.md` §i Q9).
- **Linux secret store.** No `ISecretStore` implementation; CLI auth flows
  refuse to run on Linux clients (`src/main.cpp:633-637` and siblings).
  The server runs on Linux; the *client* on Linux is unsupported as a
  result.

## Related documents

- [THREAT_MODEL.md](THREAT_MODEL.md) — STRIDE summary, AUDIT.md guardrails
  F-1 / F-2 / F-4 / F-5, residual risks.
- [RUNBOOK.md](RUNBOOK.md) — server prerequisites, deploy, master key
  handling, backups, runner ops.
- [CONTRIBUTING.md](CONTRIBUTING.md) — per-platform build, coverage,
  snapshots, commit conventions.
- [MIGRATION_V0.1_TO_V0.2.md](MIGRATION_V0.1_TO_V0.2.md) — operator path
  from the v0.1 JSON file to the off-machine server.
- [../V0_2_PLAN.md](../V0_2_PLAN.md) — the six-phase plan this code
  implements.
