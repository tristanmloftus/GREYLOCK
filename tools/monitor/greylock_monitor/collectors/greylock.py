"""Greylock activity collector — log tail, plaid status, DB counts.

This is the most config-dependent collector. Four sub-collectors that degrade
independently so a missing data source never crashes the panel.
"""

from __future__ import annotations

import asyncio
import json
import os
import sqlite3
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

from ..config import GreylockConfig

SUBPROCESS_TIMEOUT_S = 3.0

LOG_AUTODETECT_PATHS = [
    "/var/log/greylock/app.log",
    "/var/log/greylock.log",
    "/opt/greylock/logs/app.log",
    str(Path.home() / ".greylock" / "logs" / "app.log"),
]

PLAID_AUTODETECT_PATHS = [
    "/var/lib/greylock/plaid_status.json",
    "/opt/greylock/state/plaid.json",
]


@dataclass
class DbCount:
    label: str
    value: int | None
    error: str | None = None


@dataclass
class GreylockSnapshot:
    log_lines: list[str] = field(default_factory=list)
    log_source: str | None = None
    log_note: str | None = None
    plaid_status: str | None = None
    plaid_last_sync: str | None = None
    plaid_note: str | None = None
    db_counts: list[DbCount] = field(default_factory=list)
    db_note: str | None = None
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


async def collect(cfg: GreylockConfig) -> GreylockSnapshot:
    start = time.perf_counter()
    snap = GreylockSnapshot(collected_at=time.time())
    try:
        snap.log_lines, snap.log_source, snap.log_note = await _log_tail(cfg.log_path)
        snap.plaid_status, snap.plaid_last_sync, snap.plaid_note = _plaid_status(cfg.plaid_status_file)
        snap.db_counts, snap.db_note = _db_counts(cfg.db)
    except Exception as e:  # pragma: no cover
        snap.error = f"{type(e).__name__}: {e}"
    snap.duration_ms = (time.perf_counter() - start) * 1000.0
    return snap


async def _log_tail(configured_path: str | None, n: int = 20) -> tuple[list[str], str | None, str | None]:
    paths_to_try = [configured_path] if configured_path else LOG_AUTODETECT_PATHS
    for p in paths_to_try:
        if not p:
            continue
        try:
            with open(p, "r", errors="replace") as f:
                d: deque[str] = deque(f, maxlen=n)
            return [line.rstrip("\n") for line in d], p, None
        except (FileNotFoundError, IsADirectoryError):
            continue
        except PermissionError:
            return [], p, f"permission denied: {p}"
        except OSError as e:
            return [], p, f"{type(e).__name__}: {e}"
    # Fall back to journalctl
    try:
        proc = await asyncio.create_subprocess_exec(
            "journalctl", "-u", "greylock*", "-n", str(n), "--no-pager", "--output=short",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return [], None, "no log file; journalctl not installed"
    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        return [], None, "journalctl timed out"
    if proc.returncode != 0:
        err = stderr.decode("utf-8", errors="replace").strip()[:80]
        return [], None, err or "journalctl failed"
    lines = [ln for ln in stdout.decode("utf-8", errors="replace").splitlines() if ln.strip()]
    if not lines:
        return [], None, "no log file and no greylock units"
    return lines[-n:], "journalctl", None


def _plaid_status(configured_path: str | None) -> tuple[str | None, str | None, str | None]:
    paths_to_try = [configured_path] if configured_path else PLAID_AUTODETECT_PATHS
    for p in paths_to_try:
        if not p:
            continue
        try:
            with open(p, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, IsADirectoryError):
            continue
        except (PermissionError, json.JSONDecodeError, OSError) as e:
            return None, None, f"{type(e).__name__}"
        status = data.get("status") if isinstance(data, dict) else None
        last_sync = data.get("last_sync") if isinstance(data, dict) else None
        return (
            str(status) if status is not None else None,
            str(last_sync) if last_sync is not None else None,
            None,
        )
    return None, None, "no plaid status file"


def _db_counts(db) -> tuple[list[DbCount], str | None]:
    if db is None or not db.queries:
        return [], "no [greylock.db] configured"
    rows: list[DbCount] = []
    if db.kind == "sqlite":
        if not db.path:
            return [], "sqlite: missing path"
        if not os.path.exists(db.path):
            return [], f"sqlite: file not found ({db.path})"
        try:
            conn = sqlite3.connect(f"file:{db.path}?mode=ro", uri=True, timeout=2.0)
        except sqlite3.Error as e:
            return [], f"sqlite open failed: {e}"
        try:
            for label, query in db.queries.items():
                rows.append(_run_query_sqlite(conn, label, query))
        finally:
            conn.close()
        return rows, None
    if db.kind == "postgres":
        try:
            import psycopg  # type: ignore
        except ImportError:
            return [], "postgres: install greylock-monitor[postgres]"
        if not db.conninfo:
            return [], "postgres: missing conninfo"
        try:
            conn = psycopg.connect(
                db.conninfo,
                autocommit=True,
                options="-c default_transaction_read_only=on",
                connect_timeout=2,
            )
        except Exception as e:  # noqa: BLE001 — psycopg error hierarchy varies
            return [], f"postgres connect failed: {type(e).__name__}"
        try:
            for label, query in db.queries.items():
                rows.append(_run_query_postgres(conn, label, query))
        finally:
            conn.close()
        return rows, None
    return [], f"unknown db kind: {db.kind}"


def _run_query_sqlite(conn: sqlite3.Connection, label: str, query: str) -> DbCount:
    try:
        cur = conn.execute(query)
        row = cur.fetchone()
        value = int(row[0]) if row and row[0] is not None else None
        return DbCount(label=label, value=value)
    except (sqlite3.Error, ValueError, TypeError) as e:
        return DbCount(label=label, value=None, error=f"{type(e).__name__}")


def _run_query_postgres(conn, label: str, query: str) -> DbCount:
    try:
        with conn.cursor() as cur:
            cur.execute(query)
            row = cur.fetchone()
        value = int(row[0]) if row and row[0] is not None else None
        return DbCount(label=label, value=value)
    except Exception as e:  # noqa: BLE001
        print(f"greylock-monitor: db query '{label}' failed: {e}", file=sys.stderr)
        return DbCount(label=label, value=None, error=f"{type(e).__name__}")
