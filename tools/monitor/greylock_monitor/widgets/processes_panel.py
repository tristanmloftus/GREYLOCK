"""Top processes panel widget — two side-by-side mini-tables."""

from __future__ import annotations

from ..collectors import processes as proc_collector
from ._base import Panel


def _table(title: str, rows, value_key: str) -> str:
    header = f"[bold]{title}[/]\n[dim]{'PID':>6} {'USER':<10} {'%':>5}  NAME[/]"
    lines = [header]
    for r in rows:
        val = getattr(r, value_key)
        lines.append(f"{r.pid:>6} {r.user:<10} {val:5.1f}  {r.name}")
    return "\n".join(lines)


class ProcessesPanel(Panel):
    def __init__(self) -> None:
        super().__init__("Top Processes")

    async def refresh_data(self) -> None:
        try:
            snap = proc_collector.collect(limit=8)
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        cpu_tbl = _table("Top 8 by CPU", snap.top_cpu, "cpu_percent")
        mem_tbl = _table("Top 8 by RAM", snap.top_mem, "memory_percent")
        # Render stacked rather than side-by-side — terminal-width agnostic.
        self.set_body(f"{cpu_tbl}\n\n{mem_tbl}")
        self.set_footer(snap.collected_at, snap.duration_ms)
