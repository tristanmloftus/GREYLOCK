"""Config loader — TOML, user-then-system fallback, all sections optional."""

from __future__ import annotations

import os
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class DbConfig:
    kind: str  # "sqlite" or "postgres"
    path: str | None = None
    conninfo: str | None = None
    queries: dict[str, str] = field(default_factory=dict)


@dataclass
class GreylockConfig:
    log_path: str | None = None
    plaid_status_file: str | None = None
    db: DbConfig | None = None


@dataclass
class Config:
    greylock: GreylockConfig = field(default_factory=GreylockConfig)
    source_path: str | None = None  # diagnostic

    @property
    def has_greylock_section(self) -> bool:
        g = self.greylock
        return bool(g.log_path or g.plaid_status_file or g.db)


DEFAULT_PATHS = [
    Path(os.path.expanduser("~/.config/greylock-monitor/config.toml")),
    Path("/etc/greylock-monitor/config.toml"),
]


def load(path: str | os.PathLike[str] | None = None) -> Config:
    """Load config from explicit path or first default that exists.

    Missing file → empty config. Malformed file → empty config + stderr warn.
    """
    candidates: list[Path]
    if path:
        candidates = [Path(path)]
    else:
        candidates = DEFAULT_PATHS

    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            with candidate.open("rb") as f:
                raw = tomllib.load(f)
        except (OSError, tomllib.TOMLDecodeError) as e:
            print(f"greylock-monitor: failed to load {candidate}: {e}", file=sys.stderr)
            return Config()
        return _parse(raw, str(candidate))
    return Config()


def _parse(raw: dict, source: str) -> Config:
    g_raw = raw.get("greylock", {}) or {}
    db_raw = g_raw.get("db")
    db: DbConfig | None = None
    if isinstance(db_raw, dict):
        kind = db_raw.get("kind")
        if kind in ("sqlite", "postgres"):
            db = DbConfig(
                kind=kind,
                path=db_raw.get("path"),
                conninfo=db_raw.get("conninfo"),
                queries={k: v for k, v in (db_raw.get("queries") or {}).items() if isinstance(v, str)},
            )
        else:
            print(
                f"greylock-monitor: ignoring [greylock.db] — kind must be 'sqlite' or 'postgres'",
                file=sys.stderr,
            )
    return Config(
        greylock=GreylockConfig(
            log_path=g_raw.get("log_path"),
            plaid_status_file=g_raw.get("plaid_status_file"),
            db=db,
        ),
        source_path=source,
    )
