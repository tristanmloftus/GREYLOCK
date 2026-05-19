"""Top processes collector — by CPU and by RAM."""

from __future__ import annotations

import time
from dataclasses import dataclass, field

import psutil


@dataclass
class ProcInfo:
    pid: int
    user: str
    cpu_percent: float
    memory_percent: float
    name: str


@dataclass
class ProcessesSnapshot:
    top_cpu: list[ProcInfo] = field(default_factory=list)
    top_mem: list[ProcInfo] = field(default_factory=list)
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


_NAME_TRUNC = 18


def seed() -> None:
    """Prime psutil's per-process CPU sampling.

    Call once on app start. Without a prior sample, every cpu_percent() reads 0.
    """
    for p in psutil.process_iter([]):
        try:
            p.cpu_percent(interval=None)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue


def collect(limit: int = 8) -> ProcessesSnapshot:
    start = time.perf_counter()
    snap = ProcessesSnapshot(collected_at=time.time())
    try:
        rows: list[ProcInfo] = []
        for p in psutil.process_iter(["pid", "name", "username"]):
            try:
                info = p.info
                cpu = p.cpu_percent(interval=None)
                mem = p.memory_percent()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
            name = (info.get("name") or "")[:_NAME_TRUNC]
            user = (info.get("username") or "")[:12]
            rows.append(
                ProcInfo(
                    pid=info.get("pid") or 0,
                    user=user,
                    cpu_percent=cpu,
                    memory_percent=mem,
                    name=name,
                )
            )
        snap.top_cpu = sorted(rows, key=lambda r: r.cpu_percent, reverse=True)[:limit]
        snap.top_mem = sorted(rows, key=lambda r: r.memory_percent, reverse=True)[:limit]
    except Exception as e:  # pragma: no cover
        snap.error = f"{type(e).__name__}: {e}"
    snap.duration_ms = (time.perf_counter() - start) * 1000.0
    return snap
