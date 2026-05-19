"""System metrics collector — hostname, uptime, load, CPU, RAM, disk, swap."""

from __future__ import annotations

import os
import socket
import time
from dataclasses import dataclass

import psutil


@dataclass
class SystemSnapshot:
    hostname: str
    uptime_seconds: float | None = None
    load_1m: float | None = None
    load_5m: float | None = None
    load_15m: float | None = None
    cpu_percent: float | None = None
    ram_used_gib: float | None = None
    ram_total_gib: float | None = None
    disk_used_gib: float | None = None
    disk_total_gib: float | None = None
    swap_used_gib: float | None = None
    swap_total_gib: float | None = None
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


_GIB = 1024**3


def _uptime_seconds() -> float | None:
    try:
        with open("/proc/uptime", "r") as f:
            return float(f.read().split()[0])
    except (FileNotFoundError, OSError, ValueError):
        # macOS / non-Linux fallback via psutil
        try:
            return time.time() - psutil.boot_time()
        except Exception:
            return None


def collect() -> SystemSnapshot:
    start = time.perf_counter()
    snap = SystemSnapshot(hostname=socket.gethostname(), collected_at=time.time())
    try:
        snap.uptime_seconds = _uptime_seconds()
        try:
            load = os.getloadavg()
            snap.load_1m, snap.load_5m, snap.load_15m = load
        except (AttributeError, OSError):
            pass
        # Non-blocking — relies on continuous sampling between calls.
        snap.cpu_percent = psutil.cpu_percent(interval=None)
        vm = psutil.virtual_memory()
        snap.ram_used_gib = vm.used / _GIB
        snap.ram_total_gib = vm.total / _GIB
        du = psutil.disk_usage("/")
        snap.disk_used_gib = du.used / _GIB
        snap.disk_total_gib = du.total / _GIB
        sw = psutil.swap_memory()
        snap.swap_used_gib = sw.used / _GIB
        snap.swap_total_gib = sw.total / _GIB
    except Exception as e:  # pragma: no cover — defensive only
        snap.error = f"{type(e).__name__}: {e}"
    snap.duration_ms = (time.perf_counter() - start) * 1000.0
    return snap


def format_uptime(seconds: float | None) -> str:
    if seconds is None:
        return "n/a"
    days, rem = divmod(int(seconds), 86400)
    hours, rem = divmod(rem, 3600)
    minutes, _ = divmod(rem, 60)
    if days:
        return f"{days}d {hours}h {minutes}m"
    if hours:
        return f"{hours}h {minutes}m"
    return f"{minutes}m"


def pct_color(value: float | None) -> str:
    """Return a textual color name based on usage percentage."""
    if value is None:
        return "white"
    if value < 70:
        return "green"
    if value < 90:
        return "yellow"
    return "red"
