"""Auth/security collector — auth.log tail + fail2ban status."""

from __future__ import annotations

import asyncio
import shutil
import time
from collections import deque
from dataclasses import dataclass, field

SUBPROCESS_TIMEOUT_S = 3.0

AUTH_LOG_PATH = "/var/log/auth.log"
KEYWORDS = ("Accepted", "Failed", "Invalid", "session opened", "session closed")


@dataclass
class AuthLine:
    raw: str

    @property
    def kind(self) -> str:
        if "Failed" in self.raw or "Invalid" in self.raw:
            return "fail"
        if "Accepted" in self.raw or "session opened" in self.raw:
            return "ok"
        return "info"


@dataclass
class Fail2banJail:
    name: str
    currently_banned: int
    total_banned: int


@dataclass
class AuthSnapshot:
    lines: list[AuthLine] = field(default_factory=list)
    lines_note: str | None = None
    fail2ban: list[Fail2banJail] = field(default_factory=list)
    fail2ban_available: bool = False
    fail2ban_note: str | None = None
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


async def collect() -> AuthSnapshot:
    start = time.perf_counter()
    snap = AuthSnapshot(collected_at=time.time())
    try:
        snap.lines, snap.lines_note = await _auth_log_tail()
        if shutil.which("fail2ban-client"):
            snap.fail2ban_available = True
            snap.fail2ban, snap.fail2ban_note = await _fail2ban()
    except Exception as e:  # pragma: no cover
        snap.error = f"{type(e).__name__}: {e}"
    snap.duration_ms = (time.perf_counter() - start) * 1000.0
    return snap


def _filter_recent(lines, n: int = 10) -> list[AuthLine]:
    out: deque[AuthLine] = deque(maxlen=n)
    for raw in lines:
        if "sshd" not in raw:
            continue
        if not any(kw in raw for kw in KEYWORDS):
            continue
        out.append(AuthLine(raw=raw.rstrip("\n")))
    return list(out)


async def _auth_log_tail() -> tuple[list[AuthLine], str | None]:
    try:
        with open(AUTH_LOG_PATH, "r", errors="replace") as f:
            tail = deque(f, maxlen=2000)
        return _filter_recent(tail, n=10), None
    except FileNotFoundError:
        pass
    except PermissionError:
        # Fall through to journalctl
        pass
    except OSError as e:
        return [], f"{type(e).__name__}"
    # Fallback: journalctl _COMM=sshd -n 30
    try:
        proc = await asyncio.create_subprocess_exec(
            "journalctl", "_COMM=sshd", "-n", "60", "--no-pager",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return [], "n/a (need adm group)"
    try:
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except asyncio.TimeoutError:
        proc.kill()
        return [], "journalctl timed out"
    if proc.returncode != 0:
        return [], "n/a (need adm group)"
    return _filter_recent(stdout.decode("utf-8", errors="replace").splitlines(), n=10), None


async def _fail2ban() -> tuple[list[Fail2banJail], str | None]:
    jails = await _fail2ban_jail_list()
    if jails is None:
        return [], "status unavailable"
    out: list[Fail2banJail] = []
    for jail in jails:
        info = await _fail2ban_jail_status(jail)
        if info:
            out.append(info)
    return out, None


async def _fail2ban_jail_list() -> list[str] | None:
    try:
        proc = await asyncio.create_subprocess_exec(
            "fail2ban-client", "status",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except (FileNotFoundError, asyncio.TimeoutError):
        return None
    if proc.returncode != 0:
        return None
    # Expected line: "Jail list:    sshd, recidive"
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        if "Jail list:" in line:
            tail = line.split("Jail list:", 1)[1]
            return [s.strip() for s in tail.split(",") if s.strip()]
    return []


async def _fail2ban_jail_status(jail: str) -> Fail2banJail | None:
    try:
        proc = await asyncio.create_subprocess_exec(
            "fail2ban-client", "status", jail,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
    except (FileNotFoundError, asyncio.TimeoutError):
        return None
    if proc.returncode != 0:
        return None
    cur = total = 0
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        s = line.strip()
        if s.startswith("Currently banned:"):
            try:
                cur = int(s.split(":", 1)[1].strip())
            except ValueError:
                pass
        elif s.startswith("Total banned:"):
            try:
                total = int(s.split(":", 1)[1].strip())
            except ValueError:
                pass
    return Fail2banJail(name=jail, currently_banned=cur, total_banned=total)
