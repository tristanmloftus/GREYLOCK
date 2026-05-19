"""Network collector — listening ports, egress IP, optional public IP, throughput."""

from __future__ import annotations

import asyncio
import socket
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

import psutil

SUBPROCESS_TIMEOUT_S = 3.0
PUBLIC_IP_TTL_S = 600.0


@dataclass
class ListeningPort:
    proto: str
    port: int
    process: str | None


@dataclass
class NetworkSnapshot:
    listening: list[ListeningPort] = field(default_factory=list)
    listening_note: str | None = None
    egress_ip: str | None = None
    public_ip: str | None = None
    bytes_recv_per_s: float | None = None
    bytes_sent_per_s: float | None = None
    collected_at: float = 0.0
    duration_ms: float = 0.0
    error: str | None = None


class NetworkCollector:
    """Stateful — needs to remember last counters for throughput deltas."""

    def __init__(self) -> None:
        self._last_io = None  # tuple[float, int, int] — (t, recv, sent)
        self._public_ip_cache: tuple[float, str] | None = None
        self._cached_listen_note_root_hint = False

    async def collect(self, *, enable_public_ip: bool = False) -> NetworkSnapshot:
        start = time.perf_counter()
        snap = NetworkSnapshot(collected_at=time.time())
        try:
            snap.listening, snap.listening_note = await self._listening()
            snap.egress_ip = self._egress_ip()
            if enable_public_ip:
                snap.public_ip = self._public_ip()
            recv, sent = self._throughput()
            snap.bytes_recv_per_s = recv
            snap.bytes_sent_per_s = sent
        except Exception as e:  # pragma: no cover
            snap.error = f"{type(e).__name__}: {e}"
        snap.duration_ms = (time.perf_counter() - start) * 1000.0
        return snap

    async def _listening(self) -> tuple[list[ListeningPort], str | None]:
        """Run `ss -tlnp` and parse. Degrade if process column needs root."""
        try:
            proc = await asyncio.create_subprocess_exec(
                "ss", "-tlnp",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
        except FileNotFoundError:
            return [], "ss not installed"
        try:
            stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=SUBPROCESS_TIMEOUT_S)
        except asyncio.TimeoutError:
            proc.kill()
            return [], "ss timed out"
        rows: list[ListeningPort] = []
        note: str | None = None
        any_process_col = False
        for line in stdout.decode("utf-8", errors="replace").splitlines()[1:]:
            parts = line.split()
            if len(parts) < 4:
                continue
            local = parts[3]
            # local addr is "0.0.0.0:22" or "[::]:22"
            port_str = local.rsplit(":", 1)[-1]
            try:
                port = int(port_str)
            except ValueError:
                continue
            process = None
            if len(parts) >= 6:
                tail = " ".join(parts[5:])
                if "users:" in tail:
                    any_process_col = True
                    # users:(("sshd",pid=1234,fd=3))
                    try:
                        process = tail.split('"', 2)[1]
                    except IndexError:
                        process = None
            rows.append(ListeningPort(proto="tcp", port=port, process=process))
        if rows and not any_process_col:
            note = "process names require root"
        # Dedupe (multiple binds same port)
        seen = set()
        deduped = []
        for r in rows:
            key = (r.proto, r.port, r.process)
            if key in seen:
                continue
            seen.add(key)
            deduped.append(r)
        deduped.sort(key=lambda r: r.port)
        return deduped, note

    @staticmethod
    def _egress_ip() -> str | None:
        """Determine local egress IP without sending a packet (UDP socket trick)."""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("1.1.1.1", 1))
            return s.getsockname()[0]
        except OSError:
            return None
        finally:
            s.close()

    def _public_ip(self) -> str | None:
        now = time.time()
        if self._public_ip_cache and (now - self._public_ip_cache[0]) < PUBLIC_IP_TTL_S:
            return self._public_ip_cache[1]
        try:
            with urllib.request.urlopen("https://icanhazip.com", timeout=5) as resp:
                ip = resp.read().decode("utf-8").strip()
        except (urllib.error.URLError, TimeoutError, OSError):
            return None
        self._public_ip_cache = (now, ip)
        return ip

    def _throughput(self) -> tuple[float | None, float | None]:
        io = psutil.net_io_counters(pernic=False)
        now = time.time()
        if self._last_io is None:
            self._last_io = (now, io.bytes_recv, io.bytes_sent)
            return None, None
        prev_t, prev_recv, prev_sent = self._last_io
        dt = max(now - prev_t, 1e-6)
        recv = (io.bytes_recv - prev_recv) / dt
        sent = (io.bytes_sent - prev_sent) / dt
        self._last_io = (now, io.bytes_recv, io.bytes_sent)
        return recv, sent


def format_bytes_per_s(value: float | None) -> str:
    if value is None:
        return "—"
    for unit in ("B/s", "KB/s", "MB/s", "GB/s"):
        if value < 1024:
            return f"{value:6.1f} {unit}"
        value /= 1024
    return f"{value:6.1f} TB/s"
