"""Entry point.

Read-only guarantees enforced throughout this package:
  - No subprocess calls mutate state. Forbidden binaries: kill, rm, mv,
    systemctl start/stop/restart, iptables, fail2ban-client set/unban, etc.
  - sqlite handles open with `mode=ro` URI flag.
  - postgres handles open with `default_transaction_read_only=on`.
  - No log files or state files are written by the monitor itself.
  - Diagnostic output goes to stderr only.
  - The only outbound network call possible is the optional `--public-ip`
    fetch (off by default).
"""

from __future__ import annotations

import argparse
import sys

from . import __version__
from .app import GreylockMonitorApp
from .config import load as load_config


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="greylock-monitor",
        description="Read-only TUI ops monitor for the Greylock server.",
    )
    p.add_argument(
        "--public-ip",
        action="store_true",
        help="Enable one outbound HTTPS fetch to icanhazip.com (10-min cache). Off by default.",
    )
    p.add_argument(
        "--config",
        metavar="PATH",
        help="Path to config TOML (default: ~/.config/greylock-monitor/config.toml then /etc/...).",
    )
    p.add_argument(
        "--no-color",
        action="store_true",
        help="Disable theming (for piped/recorded terminals).",
    )
    p.add_argument("--version", action="version", version=f"greylock-monitor {__version__}")
    return p


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    cfg = load_config(args.config)
    app = GreylockMonitorApp(enable_public_ip=args.public_ip, config=cfg)
    if args.no_color:
        # Textual reads NO_COLOR from env; setting per-app keeps the flag local.
        import os

        os.environ["NO_COLOR"] = "1"
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
