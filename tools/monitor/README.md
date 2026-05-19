# greylock-monitor

Read-only TUI ops dashboard for the Greylock server. Headless-friendly, runs over SSH.

A **sidecar** — does not import from Greylock. Observes only via files, sockets, read-only DB handles, and standard Linux tools. No writes, no kills, no config changes.

## Install

Requires Python 3.11+ (Debian 12 ships 3.11). From an existing Greylock checkout on the server:

```bash
cd /path/to/greylock/tools/monitor
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
# Optional postgres support for db counts:
# pip install -e ".[postgres]"
```

The monitor's Python venv is fully isolated from the main Greylock C++ build. They share a repo root but nothing else.

Permissions (one-time, log out + back in after):

```bash
sudo usermod -aG adm $USER   # so the monitor can read /var/log/auth.log
```

## Run

```bash
greylock-monitor             # if you pip-installed
python -m greylock_monitor   # without install
./greylock-monitor           # bash shim in the repo root
```

Flags: `--public-ip` (one outbound HTTPS fetch, off by default, 10-min cache), `--config PATH`, `--no-color`, `--version`.

## Keys

`q` quit · `r` force refresh · `?` help overlay · `esc` close help

## Config

Optional. Place at `~/.config/greylock-monitor/config.toml` (or pass `--config PATH`). See `examples/config.toml.example` for the full schema with comments.

```toml
[greylock]
log_path = "/var/log/greylock/app.log"
plaid_status_file = "/var/lib/greylock/plaid_status.json"

[greylock.db]
kind = "sqlite"
path = "/var/lib/greylock/greylock.db"

[greylock.db.queries]
transactions_24h = "SELECT count(*) FROM transactions WHERE created_at > datetime('now', '-1 day')"
decisions_24h   = "SELECT count(*) FROM decisions   WHERE created_at > datetime('now', '-1 day')"
```

Every section is optional. Missing data sources render as "n/a" — never an error.

## Panels (3x2 grid)

| Panel | Source |
|---|---|
| System | `psutil`, `/proc/uptime`, `os.getloadavg` |
| Network | `ss -tlnp`, socket egress trick, `psutil.net_io_counters` |
| Sessions | `who`, `tmux list-sessions`, `psutil.net_connections` |
| Greylock | log file or `journalctl -u 'greylock*'`, plaid JSON, sqlite/pg read-only |
| Auth | `/var/log/auth.log` or `journalctl _COMM=sshd`, `fail2ban-client status` |
| Top Procs | `psutil.process_iter` (top 8 CPU, top 8 RAM) |

## Read-only guarantees

- No state-mutating subprocess calls. Forbidden binaries: `kill`, `rm`, `mv`, `systemctl start/stop/restart`, `iptables`, `fail2ban-client set/unban`.
- sqlite handles open with `mode=ro` URI flag.
- postgres handles open with `default_transaction_read_only=on`.
- Monitor writes zero files (no logs, no state). Diagnostics go to stderr only.
- The only outbound network call possible is the optional `--public-ip` fetch.

## Tests

```bash
pip install -e ".[dev]"
pytest -q
```

## License

MIT.
