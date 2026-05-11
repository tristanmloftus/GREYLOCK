# Runbook (server ops)

Operating guide for running `TerminalFinanceServer` on a Linux host. The
current target is the self-hosted CI host (skynet-debian); a Hetzner /
DigitalOcean VPS deployment is the v0.3 destination.

For build instructions on dev machines, see
[CONTRIBUTING.md](CONTRIBUTING.md). For the security envelope the server is
expected to enforce, see [THREAT_MODEL.md](THREAT_MODEL.md). The full
config-flag surface lives in [`../BUILD.md`](../BUILD.md).

## Server prerequisites

Tested on Debian 12 (skynet-debian VM). The same packages apply to
Ubuntu 22.04+.

```sh
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    curl \
    pkg-config \
    libsodium-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libsqlcipher-dev
```

Optional but useful:

- `gcovr` — for coverage reports if `-DTF_COVERAGE=ON` is added (currently
  not wired in `CMakeLists.txt`; see
  [CONTRIBUTING.md](CONTRIBUTING.md#coverage)).
- `mkcert` — for dev TLS certs. Production should use Let's Encrypt or a
  managed cert.

The CMake configure step uses `find_library(_SQLCIPHER_LIB sqlcipher)` with
`PATH_SUFFIXES sqlcipher` so Debian's `libsqlcipher-dev` is found at
`/usr/include/sqlcipher/sqlite3.h` without `SQLCIPHER_ROOT` being set
(`CMakeLists.txt:670-693`).

## First-time deploy

```sh
# 1. Clone the repo on the server host.
git clone https://github.com/tristanmloftus/GREYLOCK.git
cd GREYLOCK
git checkout v0.2-dev

# 2. Configure and build the server. On the skynet-debian VM (3.8 GiB RAM),
#    CI uses -j 1 to avoid the OOM-killer (.github/workflows/ci.yml:55).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target TerminalFinanceServer -j 1

# 3. Generate or install a TLS cert + key. For dev:
scripts/generate-dev-cert.sh
# Produces dev/cert.pem + dev/key.pem (gitignored). For production, use
# Let's Encrypt and point TF_SERVER_CERT_PATH / TF_SERVER_KEY_PATH at the
# fullchain + privkey.

# 4. Set the master key and Plaid creds.
export TF_MASTER_KEY="$(head -c 32 /dev/urandom | xxd -p -c 64)"
export PLAID_CLIENT_ID="..."     # from Plaid dashboard
export PLAID_SECRET="..."

# 5. Boot. Default port 8443, default bind 127.0.0.1.
./build/TerminalFinanceServer
```

Expected startup output:

```
Database opened with master key: yes
TerminalFinance server v0.2
  bind_addr : 127.0.0.1
  port      : 8443
  cert_path : dev/cert.pem
  key_path  : dev/key.pem
  db_path   : dev/terminalfinance.db
Database migrations applied.
PlaidTokenBroker initialized (TF_MASTER_KEY present).
Listening on https://127.0.0.1:8443  (Ctrl-C to exit)
```

`server/main.cpp:323-451` is the boot sequence.

## Configuration (env vars)

| Variable | Default | Purpose | Source |
|----------|---------|---------|--------|
| `TF_SERVER_PORT` | `8443` | TCP listen port | `server/main.cpp:265` |
| `TF_SERVER_CERT_PATH` | `dev/cert.pem` | TLS cert (PEM) | `server/main.cpp:266` |
| `TF_SERVER_KEY_PATH` | `dev/key.pem` | TLS private key (PEM) | `server/main.cpp:267` |
| `TF_SERVER_BIND_ADDR` | `127.0.0.1` | Bind address (`0.0.0.0` = all NICs) | `server/main.cpp:268` |
| `TF_DB_PATH` | `dev/terminalfinance.db` | SQLCipher database file | `server/main.cpp:270` |
| `TF_MASTER_KEY` | (unset) | 64 hex chars (32 bytes); enforces encryption when set; see policy below | `server/main.cpp:81-87,196-320` |
| `TF_SUPPLIER_MAP_PATH` | `data/supplier_map.json` | Source for `GET /supplier-map` | `server/main.cpp:434-437` |
| `PLAID_CLIENT_ID` | (unset) | Plaid API client id; if unset, scheduler does not start | `server/main.cpp:394-415` |
| `PLAID_SECRET` | (unset) | Plaid API secret; same gate | `server/main.cpp:394-415` |

### Master-key policy (server side)

`server/main.cpp:285-320` enforces:

- `TF_MASTER_KEY` set → use it (`SQLCipher::sqlcipher` keyed; Plaid broker
  enabled).
- `TF_MASTER_KEY` unset, DB file missing → create unencrypted (dev
  fallback); warn loudly on stderr.
- `TF_MASTER_KEY` unset, DB file is plain SQLite (magic header
  `"SQLite format 3\x00"`) → open unencrypted; warn.
- `TF_MASTER_KEY` unset, DB file exists and is **not** plain SQLite
  (likely SQLCipher) → **hard-fail**. This prevents silently corrupting
  an encrypted database or operating with a key mismatch.

The "Database opened with master key: yes|no" line is the operator-visible
state indicator (`server/main.cpp:323-324`).

## Enrolling a user

Two-step flow (mirrors `BUILD.md:160-181`):

```sh
# 1. On the server host: mint a one-shot enrollment token for the new user's
#    email. TF_DB_PATH must match the running server. The command prints
#    only the raw token to stdout — copy it.
TF_DB_PATH=dev/terminalfinance.db \
TF_MASTER_KEY="$TF_MASTER_KEY" \
./build/TerminalFinanceServer --mint-enrollment-token alice@example.com
# Prints: <64-hex token>
```

```sh
# 2. On the user's client machine (macOS or Windows): enroll using the token.
export TF_BACKEND_URL=https://<server-host>:8443
export TF_USER_EMAIL=alice@example.com
./TerminalFinance --enroll <64-hex-token-from-step-1>
# Prompts for a new passphrase twice; on success prints the otpauth:// URI
# to scan into the user's authenticator app.
```

Source: `src/main.cpp:372-414` (`cmd_enroll`),
`server/auth/AuthHandlers.cpp:607-624` (`/auth/enroll`).

A `--list-users` admin command exists for sanity-checking enrollment
(`server/main.cpp:231-246`).

## Master-key handling

The master key is the most valuable secret in the system. Lose it and you
lose every Plaid token and the on-disk DB. Leak it and the at-rest
encryption is worthless.

### Generate

```sh
# Either:
head -c 32 /dev/urandom | xxd -p -c 64

# Or, on a host with libsodium installed:
python3 -c "import secrets; print(secrets.token_hex(32))"
```

The server expects 64 hex characters (32 bytes). 32 raw bytes is also
accepted by `PlaidTokenBroker` for convenience (`PlaidTokenBroker.cpp:104-131`),
but `Database` parses hex only (see the constructor in
`server/db/Database.cpp`). Use hex.

### Backup

`TF_MASTER_KEY` is **not** stored in the database. If you lose it, the
encrypted blobs become unrecoverable. Recommended backup procedure:

1. Print the hex string.
2. Seal it in two physically separate locations (e.g., a paper safe at
   each operator's home).
3. Optionally store it in a password manager whose master credential is
   itself backed up.

### Rotate

**No support in v0.2.** Rotation requires `PRAGMA rekey` against the
SQLCipher DB *and* re-encrypting every Plaid token blob under a new DEK.
Neither operation is automated. Plan deferred to v0.3 (`V0_2_PLAN.md`
§i Q3).

## Backup

Daily backup target: the SQLCipher database file (`TF_DB_PATH`,
default `dev/terminalfinance.db`) and the master key kept offline.

```sh
# Example cron line (run as the service user, not root):
# 0 3 * * *  /usr/local/bin/tf-backup.sh
#
# tf-backup.sh:
#   set -e
#   SRC=/var/lib/terminalfinance/terminalfinance.db
#   DST=/srv/backups/terminalfinance/$(date +%Y%m%d).db
#   install -m 600 "$SRC" "$DST"
#   # SQLCipher writes are atomic via SQLite; cp during writes is safe.
#   # If you want absolute safety, run sqlcipher .backup instead:
#   #   sqlcipher "$SRC" ".backup '$DST'"
```

To restore: copy the backup back into `TF_DB_PATH` and set the same
`TF_MASTER_KEY`. **The DB file alone is not enough** — both are required.

## Healthcheck

`GET /healthz` returns `{"ok":true,"version":"0.2"}` with HTTP 200
(`server/http/HealthzHandler.cpp:13-...`). The endpoint is unauthenticated.

```sh
curl --cacert "$(mkcert -CAROOT)/rootCA.pem" https://localhost:8443/healthz
# {"ok":true,"version":"0.2"}
```

Failure interpretations:

| Symptom | Likely cause | First action |
|---------|--------------|--------------|
| `curl: (7) Failed to connect` | Server not running, port not listening | `systemctl status terminalfinance` / check the process |
| `curl: (60) SSL certificate problem` | Cert chain not trusted; CA not installed | `--cacert <path>` or `mkcert -install`; verify `TF_SERVER_CERT_PATH` |
| HTTP 404 | Server alive but wrong path or wrong port | Confirm path is exactly `/healthz`; check `TF_SERVER_PORT` |
| HTTP 5xx | Server crashed mid-response (rare) | `journalctl -u terminalfinance -f`; capture stderr |

## Self-hosted CI runner

The Linux CI matrix runs on a self-hosted Debian VM under the
project owner's UTM/QEMU setup. The CI workflow targets
`runs-on: [self-hosted, linux, skynet]`
(`.github/workflows/ci.yml:22`).

The following references describe the runner's operating environment as
configured by the owner. The exact service name, watchdog script path,
and admin SSH alias are operator-supplied and not committed to the
repository; treat them as `[TODO verify]` against the live host before
acting.

- **Service name (typical):** `actions.runner.<owner>-GREYLOCK.<runner-name>.service`
  — `[TODO verify]` — confirm with `sudo systemctl list-units 'actions.runner.*'`.
- **Watchdog cron (typical):** `/home/<runner-user>/runner-watchdog.sh`
  — `[TODO verify]` — verify with `crontab -l -u <runner-user>`. The
  watchdog restarts the runner agent if its TCP connection to GitHub
  drops.
- **Admin SSH (typical):** `ssh <user>@skynet-debian` over Tailscale tailnet.
- **Helper (typical):** `notify-tristan "msg"` broadcasts to all open
  terminal sessions on the host.

### Restart the runner

```sh
sudo systemctl restart 'actions.runner.*'
```

### View runner logs

```sh
journalctl -u 'actions.runner.*' -f
```

## Common failure modes

### Runner agent loses connection to GitHub

Symptom: jobs queue but never start; `actions/runner` log shows
`Connection failed` or similar.

Causes seen on the skynet-debian VM:

- **Host Mac sleeping.** The VM is hosted on a Mac (UTM). When the Mac
  goes to sleep, the VM pauses; the runner's TCP connection eventually
  times out. Fix: `caffeinate` the Mac (or change its power settings) so
  it does not sleep during CI windows.
- **`opencode` (or other dev tools) eating RAM on the VM.** The VM has
  3.8 GiB RAM (CI compile uses `-j 1` for this reason —
  `.github/workflows/ci.yml:55`). Don't run heavy interactive tools on
  the VM during a build.
- **Watchdog disabled.** Confirm `crontab -l` includes the watchdog
  entry; re-add if missing.

Mitigation: bump the VM to 6 GB+ RAM in UTM and re-enable `-j 2` in
`ci.yml` (the comment at `ci.yml:53-54` already flags this).

### `TF_MASTER_KEY` mismatch on restart

Symptom: server fails to start with `SQLCipher: HMAC mismatch` or
"file is not a database" errors after a restart.

Cause: a different key was passed than the one that originally encrypted
the file.

Fix: restore the correct key from your offline backup; do **not** delete
the database file — that's data loss. If the wrong key was intentionally
set, swap it back and the server boots cleanly.

### Plaid sync silently disabled

Symptom: no audit-log entries with `actor_kind="sync_worker"`; transactions
not updating.

Cause: one of `PLAID_CLIENT_ID`, `PLAID_SECRET`, or `TF_MASTER_KEY` is
unset (`server/main.cpp:399-414` gate). Server still boots; only the
scheduler is suppressed.

Fix: set all three env vars and restart. Confirm
`PlaidSyncScheduler: not started` is **not** in the boot log.

## Related documents

- [ARCHITECTURE.md](ARCHITECTURE.md) — what the server is doing, in code
  terms.
- [THREAT_MODEL.md](THREAT_MODEL.md) — what the server is supposed to
  defend against and what it is not.
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to build, test, and run
  coverage on a dev box.
- [MIGRATION_V0.1_TO_V0.2.md](MIGRATION_V0.1_TO_V0.2.md) — onboarding a
  v0.1 operator after the server is up.
- [`../BUILD.md`](../BUILD.md) — full per-platform recipe (kept as the
  deep cookbook).
