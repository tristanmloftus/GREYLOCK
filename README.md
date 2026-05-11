[![CI](https://github.com/tristanmloftus/GREYLOCK/actions/workflows/ci.yml/badge.svg?branch=v0.2-dev)](https://github.com/tristanmloftus/GREYLOCK/actions/workflows/ci.yml)

# TerminalFinance (GREYLOCK)

TerminalFinance is a terminal-UI personal and small-business finance tracker.
It pairs an FTXUI client with an off-machine HTTPS backend, syncs accounts
through Plaid server-side, and audit-logs every sensitive operation against
a BLAKE2b-chained ledger. Data at rest is encrypted with SQLCipher; Plaid
access tokens are wrapped in libsodium envelope encryption with AAD bound to
`account_id`.

## Status

v0.2 in development on the `v0.2-dev` branch. Target platforms:

- **Linux** — primary CI target. Self-hosted runner (`skynet`) green at
  `4b4c861`. Server runs on Linux.
- **macOS** — supported via Homebrew (see [Brewfile](Brewfile)). Used for
  client and server development.
- **Windows** — supported via vcpkg manifest (see [vcpkg.json](vcpkg.json)).
  Client-only in practice; the v0.1 DPAPI secret store is preserved.

Six implementation phases are tracked in [V0_2_PLAN.md](V0_2_PLAN.md);
Phase 5 (TUI + Discovery + Consolidation) is the most recent shipped work.

## Quick start

```sh
# 1. Install dependencies (macOS shown; Linux/Windows in docs/CONTRIBUTING.md).
brew bundle --file=Brewfile

# 2. Configure and build (server + client + tests).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4

# 3. Run the 28 ctest targets.
ctest --test-dir build --output-on-failure
```

Per-platform deps and the full developer workflow (Linux apt packages,
Windows vcpkg toolchain, coverage builds, snapshot updates) live in
[docs/CONTRIBUTING.md](docs/CONTRIBUTING.md). Operating a server is in
[docs/RUNBOOK.md](docs/RUNBOOK.md).

## Architecture in one diagram

```
+----------------------+         HTTPS         +-------------------------+
|   TerminalFinance    |  ------------------>  |  TerminalFinanceServer  |
|   (FTXUI TUI client) |  Authorization:       |  (cpp-httplib + TLS)    |
|                      |    Bearer <token>     |                         |
|  - DashboardView     |  <------------------  |  - /auth/*              |
|  - AccountsView      |                       |  - /entities/...        |
|  - TransactionsView  |                       |  - /accounts/...        |
|  - BudgetView        |                       |  - /transactions/...    |
|                      |                       |  - /categories/...      |
|  ServiceContainer:   |                       |  - /budgets/...         |
|   - BackendClient    |                       |  - /supplier-map        |
|   - AuthService      |                       |  - /healthz             |
|   - ISecretStore     |                       |                         |
|     (DPAPI/Keychain) |                       +-----------+-------------+
|   - IHttpClient      |                                   |
|     (libcurl, HTTPS) |                                   |
+----------------------+                       +-----------+-------------+
                                               |   SQLCipher (AES-256)   |
                                               |   data.db               |
                                               |  - users / sessions     |
                                               |  - entities / accounts  |
                                               |  - transactions / ...   |
                                               |  - audit_log (BLAKE2b)  |
                                               |  - plaid_sync_state     |
                                               +-----------+-------------+
                                                           |
                                                           v
                                               +-------------------------+
                                               |  PlaidTokenBroker       |
                                               |  (XChaCha20-Poly1305    |
                                               |   AAD=account_id)       |
                                               +-----------+-------------+
                                                           |
                                                           v
                                               +-------------------------+
                                               |  Plaid /transactions/   |
                                               |  sync (15-min cron)     |
                                               +-------------------------+
```

Detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Security in one paragraph

Passphrases are hashed with libsodium Argon2id at `OPSLIMIT_MODERATE` (3
ops) + `MEMLIMIT_MODERATE` (256 MiB), via `crypto_pwhash_str`
(`server/auth/PassphraseHash.cpp:33`). Login is gated by passphrase + TOTP
(RFC 6238, `server/auth/Totp.cpp`). Sessions are 32 random bytes
(`randombytes_buf`) base64url-encoded for transit and BLAKE2b-256-hashed
for storage (`server/auth/Session.cpp:76-97`); idle timeout is 30 minutes,
absolute timeout 8 hours (`server/auth/Session.h:35-36`). Plaid access
tokens are wrapped in libsodium XChaCha20-Poly1305 IETF with
`AAD = account_id` bytes
(`server/plaid/PlaidTokenBroker.cpp:158-190`,
`src/services/crypto/EnvelopeEncryption.h`); the master key (32 bytes via
`TF_MASTER_KEY`) is consumed once to derive the DEK and zeroed
immediately (`server/plaid/PlaidTokenBroker.cpp:74,140`). The on-disk
database is SQLCipher (AES-256) — keyed from `TF_MASTER_KEY` at server
boot (`server/main.cpp:81-87,335-347`). The audit log is BLAKE2b-256
chained with a canonical byte layout that explicitly avoids the AUDIT.md
F-4 timestamp double-count (`server/audit/CanonicalBytes.cpp:65-92`).
Threat model and residual risks in
[docs/THREAT_MODEL.md](docs/THREAT_MODEL.md).

## Tests

```sh
cmake --build build && ctest --test-dir build --output-on-failure
```

28 `add_test` targets at `4b4c861`
(`grep -c '^add_test' CMakeLists.txt`). One additional target,
`SecretStoreTests`, is conditional on macOS/Windows
(`CMakeLists.txt:443`).

Snapshot tests for the seven FTXUI widgets under `src/views/widgets/`
live in `tests/snapshot/` (rendered at fixed `80x24`, diffed against
checked-in fixtures). To refresh fixtures after an intentional widget
change:

```sh
TF_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R WidgetSnapshot --output-on-failure
```

See [tests/snapshot/README.md](tests/snapshot/README.md) for the harness
contract.

## Repo map

```
.
+- src/                      TUI client code (links to TerminalFinance binary)
|  +- main.cpp               CLI entry; --enroll / --login / --logout /
|  |                         --whoami / --migrate-from-local + FTXUI loop
|  +- models/                DataStore, Entity, Account, Transaction, ...
|  +- services/              BackendClient, AuthService, PlaidService,
|  |  |                       DiscoveryService, ConsolidationService, ...
|  |  +- crypto/             EnvelopeEncryption, ZeroizingBuffer, ConstantTime
|  |  +- http/               CurlHttpClient (HTTPS-only, CRLF-guarded)
|  |  +- secret_store/       DpapiSecretStore (Windows), KeychainSecretStore (macOS)
|  +- views/                 FTXUI views (Dashboard, Accounts, Transactions, Budget)
|  |  +- widgets/            Promoted from proposals/ in Phase 5; 7 widgets
|  +- migration/             V01Migrator (v0.1 JSON -> v0.2 server CRUD)
|  +- utils/                 Logger, ConfigManager, Validator, SyntheticGenerator
+- server/                   HTTPS server code (links to TerminalFinanceServer)
|  +- main.cpp               Boot, env-var policy, SIGINT handler, route wiring
|  +- http/                  Server (cpp-httplib SSLServer), HealthzHandler
|  +- auth/                  PassphraseHash, Totp, EnrollmentToken, Session,
|  |                          AuthHandlers, SessionMiddleware
|  +- audit/                 CanonicalBytes, Sanitizer, SqlAuditLog, StubAuditLog
|  +- data/                  Entities/Accounts/Transactions/Categories/Budgets handlers
|  +- db/                    Database, Migrations (M001-M004, SQLCipher-aware)
|  +- plaid/                 PlaidTokenBroker, PlaidApiClient, PlaidSyncScheduler
|  +- discovery/             SupplierMapHandler (serves data/supplier_map.json)
+- tests/                    GoogleTest suite (28 add_test() targets) + snapshot/
+- data/                     supplier_map.json (canonical merchant -> ticker rules)
+- scripts/                  generate-dev-cert.sh (mkcert wrapper)
+- docs/                     ARCHITECTURE, THREAT_MODEL, RUNBOOK, CONTRIBUTING,
+                             MIGRATION_V0.1_TO_V0.2
+- .github/workflows/ci.yml  Linux self-hosted runner (label: skynet)
+- CMakeLists.txt            Build system (vcpkg on Windows, Brew on macOS, apt on Linux)
+- Brewfile                  macOS deps (libsodium, curl, openssl@3, mkcert, sqlcipher)
+- vcpkg.json                Windows deps (libsodium, curl, openssl, sqlcipher)
+- BUILD.md                  Per-platform build recipes (kept as the deep cookbook)
+- V0_2_PLAN.md              The six-phase v0.2 plan (do not delete)
```

## License / authors

No license file is shipped in this repository. The previous v0.1 README
identified the project as `TerminalFinance (GREYLOCK)` with no license
clause; that status is preserved unchanged.
