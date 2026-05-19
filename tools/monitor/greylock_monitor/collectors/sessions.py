"""Sessions collector — SSH users, tmux sessions, established TCP groups."""

from __future__ import annotations

import asyncio
import time
from collections import Counter
from dataclasses import dataclass, field

import psutil

SUBPROCESS_TIMEOUT_S = 3.0


@dataclass
class SshSession:
    user: str
    tty: str
    source: str
    since: str


@dataclass
class TmuxSession:
    name: str
    windows: int
    attached: bool


@dataclass
class TcpGroup:
    remote_ip: str
    count: int


@dataclass
class SessionsSnapshot:
    ssh: list[SshSession] = field(default_factory=list)
    tmux: list[TmuxSession] = field(default_factory=list)
    tmux_note: str | None = None
    tcp_groups: list[TcpGroup] = field(default_factory=list)
    tcp_note: str | None = None
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


async def collect() -> SessionsSnapshot:
    start = time.perf_counter()
    snap = SessionsSnapshot(collected_at=time.time())
    try:
        snap.ssh = await _ssh_sessions()
        snap.tmux, snap.tmux_note = await _tmux_sessions()
        snap.tcp_groups, snap.tcp_note = _tcp_groups()
    except Exception as e:  # pragma: no cover
        snap.error = f"{type(e).__name__}: {e}"
    snap.duration_ms = (time.perf_counter() - start) * 1000.0
    return snap


async def _ssh_sessions() -> list[SshSession]:
    try:
        proc = await asyncio.create_subprocess_exec(
            "who",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return []
    try:
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        return []
    rows: list[SshSession] = []
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        user = parts[0]
        tty = parts[1]
        source = ""
        since = ""
        # `who` format varies; common: user tty MMM DD HH:MM (source)
        for part in parts[2:]:
            if part.startswith("(") and part.endswith(")"):
                source = part.strip("()")
            elif ":" in part and len(part) <= 5:
                since = part
        rows.append(SshSession(user=user, tty=tty, source=source or "local", since=since))
    return rows


async def _tmux_sessions() -> tuple[list[TmuxSession], str | None]:
    try:
        proc = await asyncio.create_subprocess_exec(
            "tmux", "list-sessions",
            "-F", "#{session_name}|#{session_windows}|#{session_attached}",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return [], "tmux not installed"
    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        return [], "tmux timed out"
    if proc.returncode != 0:
        err = stderr.decode("utf-8", errors="replace").strip()
        if "no server running" in err.lower():
            return [], None  # clean empty
        return [], err[:60] or "tmux error"
    rows: list[TmuxSession] = []
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        parts = line.split("|")
        if len(parts) != 3:
            continue
        try:
            windows = int(parts[1])
        except ValueError:
            windows = 0
        rows.append(TmuxSession(name=parts[0], windows=windows, attached=parts[2] == "1"))
    return rows, None


def _tcp_groups(limit: int = 5) -> tuple[list[TcpGroup], str | None]:
    try:
        conns = psutil.net_connections(kind="tcp")
    except (psutil.AccessDenied, PermissionError):
        return [], "needs root for full visibility"
    counter: Counter[str] = Counter()
    for c in conns:
        if c.status != psutil.CONN_ESTABLISHED:
            continue
        if c.raddr:
            counter[c.raddr.ip] += 1
    top = counter.most_common(limit)
    return [TcpGroup(remote_ip=ip, count=n) for ip, n in top], None
