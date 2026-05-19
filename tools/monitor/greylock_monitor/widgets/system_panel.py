"""System panel widget."""

from __future__ import annotations

from ..collectors import system as sys_collector
from ._base import Panel


def _row(label: str, value: str) -> str:
    return f"[bold]{label:<10}[/] {value}"


def _bar(used: float | None, total: float | None) -> str:
    if used is None or total is None or total <= 0:
        return "n/a"
    pct = (used / total) * 100
    color = sys_collector.pct_color(pct)
    return f"[{color}]{used:5.1f}[/] / {total:5.1f} GiB  [{color}]{pct:5.1f}%[/]"


class SystemPanel(Panel):
    def __init__(self) -> None:
        super().__init__("System")

    async def refresh_data(self) -> None:
        try:
            snap = sys_collector.collect()
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        cpu_color = sys_collector.pct_color(snap.cpu_percent)
        cpu_txt = f"[{cpu_color}]{snap.cpu_percent:5.1f}%[/]" if snap.cpu_percent is not None else "n/a"
        load_txt = (
            f"{snap.load_1m:.2f}  {snap.load_5m:.2f}  {snap.load_15m:.2f}"
            if snap.load_1m is not None
            else "n/a"
        )
        lines = [
            _row("host", snap.hostname),
            _row("uptime", sys_collector.format_uptime(snap.uptime_seconds)),
            _row("load", load_txt),
            _row("cpu", cpu_txt),
            _row("ram", _bar(snap.ram_used_gib, snap.ram_total_gib)),
            _row("disk /", _bar(snap.disk_used_gib, snap.disk_total_gib)),
            _row("swap", _bar(snap.swap_used_gib, snap.swap_total_gib)),
        ]
        self.set_body("\n".join(lines))
        self.set_footer(snap.collected_at, snap.duration_ms)
