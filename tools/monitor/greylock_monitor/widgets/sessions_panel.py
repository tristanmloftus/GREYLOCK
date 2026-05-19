"""Sessions panel widget."""

from __future__ import annotations

from ..collectors import sessions as sess_collector
from ._base import Panel


class SessionsPanel(Panel):
    def __init__(self) -> None:
        super().__init__("Sessions")

    async def refresh_data(self) -> None:
        try:
            snap = await sess_collector.collect()
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        lines = []
        lines.append(f"[bold]ssh[/] ({len(snap.ssh)})")
        if snap.ssh:
            for s in snap.ssh[:6]:
                lines.append(f"  {s.user:<10} {s.tty:<8} {s.source:<18} {s.since}")
        else:
            lines.append("  [dim](none)[/]")
        lines.append("")
        lines.append(f"[bold]tmux[/] ({len(snap.tmux)})")
        if snap.tmux:
            for t in snap.tmux[:6]:
                marker = "*" if t.attached else " "
                lines.append(f"  {marker} {t.name:<14} {t.windows} win")
        elif snap.tmux_note:
            lines.append(f"  [dim]{snap.tmux_note}[/]")
        else:
            lines.append("  [dim](no server)[/]")
        lines.append("")
        lines.append(f"[bold]established tcp[/] (top {len(snap.tcp_groups)})")
        if snap.tcp_groups:
            for g in snap.tcp_groups:
                lines.append(f"  {g.count:>3}  {g.remote_ip}")
        elif snap.tcp_note:
            lines.append(f"  [dim]{snap.tcp_note}[/]")
        else:
            lines.append("  [dim](none)[/]")
        self.set_body("\n".join(lines))
        self.set_footer(snap.collected_at, snap.duration_ms)
