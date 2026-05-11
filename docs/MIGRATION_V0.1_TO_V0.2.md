# Migrating from v0.1 to v0.2

This guide walks an operator from the v0.1 single-file local JSON store
(`data.json`) to the v0.2 off-machine SQLCipher-backed server. The
migration is **one-way**: keep your v0.1 JSON file as a rollback artifact
until you have verified everything you care about is present in v0.2.

System context: [ARCHITECTURE.md](ARCHITECTURE.md). Server operating
guide: [RUNBOOK.md](RUNBOOK.md). Build steps for the new client:
[CONTRIBUTING.md](CONTRIBUTING.md).

## What changed

| Aspect | v0.1 | v0.2 |
|--------|------|------|
| Storage | Single local JSON file (plaintext) | Off-machine SQLCipher DB on a server |
| Auth | None — anyone with the binary and the JSON file is "the user" | Per-user passphrase + TOTP (RFC 6238); session cached in OS secret store |
| Plaid | Plaintext access tokens persisted on `Account.plaid_access_token` | Server-side `PlaidTokenBroker`; XChaCha20-Poly1305 envelope encryption with AAD bound to `account_id`; tokens never plaintext outside one broker scope |
| Audit | None | BLAKE2b-256 hash-chained audit log on every sensitive op (`server/audit/`) |
| Plaid sync | Only when the TUI is open and links manually | Server-side 15-minute cron (`PlaidSyncScheduler`) |
| Schema | `DataStore` JSON object | Versioned migrations (`server/db/Migrations.cpp`: M001-M004) |

## What you need before migrating

Before running `--migrate-from-local`, ensure:

1. **A v0.2 server is deployed and reachable.** Follow
   [RUNBOOK.md](RUNBOOK.md). At minimum:
   - `TF_MASTER_KEY` is set and backed up.
   - `TF_SERVER_*` env vars and TLS cert are configured.
   - `curl https://<server>/healthz` returns
     `{"ok":true,"version":"0.2"}`.
2. **Your user is enrolled on the new server.** From the server host:
   ```sh
   TF_MASTER_KEY="$TF_MASTER_KEY" \
   ./build/TerminalFinanceServer --mint-enrollment-token you@example.com
   ```
   From your client:
   ```sh
   export TF_BACKEND_URL=https://<server-host>:8443
   export TF_USER_EMAIL=you@example.com
   ./TerminalFinance --enroll <minted-token>
   # Scan the otpauth:// URI into your authenticator app.
   ```
3. **You can log in and stay logged in.** This caches a session token in
   the OS secret store under the key `tf-session-<email>` — the
   migration tool reads this key to attach the Bearer token to every
   migration request.
   ```sh
   ./TerminalFinance --login
   ./TerminalFinance --whoami    # confirms session is cached
   ```
4. **A v0.2 client binary is built.** See
   [CONTRIBUTING.md](CONTRIBUTING.md). The migration command is part of
   the `TerminalFinance` client binary (not the server).
5. **Your v0.1 JSON file is intact and readable.** This was typically at
   `~/Library/Application Support/TerminalFinance/data.json` on macOS
   or `%APPDATA%\TerminalFinance\data.json` on Windows. The
   `--migrate-from-local` flag takes a path, so any location is fine.

## The migration command

```sh
export TF_BACKEND_URL=https://<server-host>:8443
export TF_USER_EMAIL=you@example.com

./TerminalFinance --migrate-from-local /path/to/v0.1/data.json
```

Source: `src/main.cpp:484-565` (`cmd_migrate_from_local`),
`src/migration/V01Migrator.cpp` (the migrator itself), and
`src/migration/V01Migrator.h` (the report struct).

The tool:

1. Verifies a cached session exists
   (`AuthService::has_cached_session`, `src/main.cpp:497-509`). If not,
   it exits with a `Run --login first.` error.
2. Reads the session token from `ISecretStore` under
   `tf-session-<TF_USER_EMAIL>` (`src/main.cpp:512-528`).
3. Reads and parses the v0.1 JSON file
   (`V01Migrator::migrate`, `src/migration/V01Migrator.cpp:55-89`).
4. Walks each collection (entities → accounts → transactions →
   categories → budgets) and POSTs each row to the corresponding v0.2
   endpoint with the session bearer attached
   (`V01Migrator.cpp:95-291`).
5. Maps each response to `created` / `skipped (409 already exists)` /
   `error` (`V01Migrator.cpp:297-328`).
6. Prints a per-collection report and the per-row error list (if any),
   then exits with status 0 if `errors == 0`, else status 1.

## What gets migrated

| v0.1 collection | v0.2 endpoint | Source mapping |
|-----------------|---------------|----------------|
| `entities[]` | `POST /entities` | `migrate_entities`, `V01Migrator.cpp:95-131`. v0.1 `type` is renamed to v0.2 `kind`. |
| `accounts[]` | `POST /entities/<entity_id>/accounts` | `migrate_accounts`, `V01Migrator.cpp:133-172`. `name`, `type`, `institution`, `plaid_item_id`, `is_active`. |
| `transactions[]` | `POST /accounts/<account_id>/transactions` | `migrate_transactions`, `V01Migrator.cpp:174-219`. `date`, `amount`, `description`, `category_id`, `pending`, `plaid_transaction_id`, `notes`, `check_number`. |
| `categories[]` | `POST /categories` | `migrate_categories`, `V01Migrator.cpp:221-255`. `name`, `type`, `emoji`, `parent_id`, `is_system`. |
| `budgets[]` | `POST /budgets` | `migrate_budgets`, `V01Migrator.cpp:257-291`. `entity_id`, `category_id`, `month`, `limit_amount`. `spent_amount` is computed server-side and is not migrated. |

Supplier mappings live in the server's canonical
`data/supplier_map.json` (served read-only at `GET /supplier-map`). They
are **not** read from the v0.1 file; if you had local overrides, port
them into `data/supplier_map.json` and redeploy.

## What gets dropped (intentionally)

- **`plaid_access_token`.** v0.1 stored Plaid access tokens plaintext on
  `Account.plaid_access_token`. The migrator **deliberately omits this
  field** from the `POST /entities/.../accounts` body
  (`V01Migrator.cpp:148-157`). This is a security guardrail: tokens must
  be re-vaulted through the new server-side `PlaidTokenBroker`, which
  envelope-encrypts them with AAD bound to `account_id`. After
  migration, you must re-link Plaid for each previously-linked account
  through the server's Plaid linking flow.
- **`spent_amount` on budgets.** Calculated server-side from
  transactions; migrating it would create stale data
  (`V01Migrator.cpp:277`).

If your v0.1 file contains other ad-hoc fields, they are silently
ignored. The migrator does not strip them from the v0.1 file — your
backup remains as-is.

## Conflict handling

The server-side handlers return HTTP 409 when a row with the same `id`
already exists (e.g., from a previous migration run). The migrator
treats 409 as **success-by-skip**, not an error
(`V01Migrator.cpp:312-317`). Each row is therefore idempotent: running
the migration twice produces the same final server state with the
second pass reporting `skipped` for everything that already migrated.

Any other non-2xx status (transport, 400, 401, 404, 5xx) is recorded as
an error and the migration continues. The error count is printed at the
end:

```
--- Migration Report ---
Entities:     12 created, 0 skipped
Accounts:     34 created, 0 skipped
Transactions: 1842 created, 0 skipped
Categories:   14 created, 0 skipped
Budgets:      0 created, 0 skipped
Errors:       0
```

If `Errors: > 0`, scroll up to the `Error details:` section. Common
causes:

- `HTTP 401 [unauthorized] ...` — session expired mid-run. Re-`--login`
  and re-run the migrator; the 409-on-skip logic will resume from where
  it stopped.
- `HTTP 404 [not_found] ...` on a transaction — the migrator tried to
  POST to `/accounts/<id>/transactions` for an account that did not
  migrate. Migrate accounts first, then re-run.
- `HTTP 400 [missing_fields] ...` — your v0.1 row was missing a field
  the server requires; fix or skip that row.

## Verifying the migration

After a clean run:

```sh
# Counts (rough): use whoami + spot-check a few entities.
./TerminalFinance --whoami

# Launch the TUI and confirm the Dashboard renders accounts + recent
# transactions that match what you had in v0.1.
TF_USER_EMAIL=you@example.com ./TerminalFinance
```

For each previously-Plaid-linked account, re-trigger the Plaid linking
flow so a token is re-vaulted server-side. Until you do, the
server-side scheduler will not include the account in its 15-minute
sync.

## Rollback

The migration is non-destructive on the client side: your v0.1 JSON
file is read but not modified. To roll back:

1. Continue using the v0.1 client binary against your local JSON file.
2. Decide whether to delete the server-side rows or leave them in
   place. If you keep them, a future re-run of the migrator will
   correctly report everything as `skipped (already exists)`.

There is no "undo" command on the server side in v0.2. To wipe the
server fresh, stop the server, delete `TF_DB_PATH`, and start it again
— it will run M001-M004 on an empty file. **This also deletes all user
accounts, sessions, and the audit chain.** Only do this if you mean it
and your master key and JSON file are both safely backed up.

## Related documents

- [ARCHITECTURE.md](ARCHITECTURE.md) — what the v0.2 system looks like.
- [RUNBOOK.md](RUNBOOK.md) — how to bring the server up before you can
  migrate.
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to build the v0.2 client
  binary that ships `--migrate-from-local`.
- [THREAT_MODEL.md](THREAT_MODEL.md) — why the migrator drops
  `plaid_access_token`.
