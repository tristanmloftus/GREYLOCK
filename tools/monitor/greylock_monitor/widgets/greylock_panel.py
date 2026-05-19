"""Greylock activity panel widget."""

from __future__ import annotations

from ..collectors import greylock as gl_collector
from ..config import GreylockConfig
from ._base import Panel


class GreylockPanel(Panel):
    def __init__(self, cfg: GreylockConfig) -> None:
        super().__init__("Greylock")
        self._cfg = cfg

    async def refresh_data(self) -> None:
        try:
            snap = await gl_collector.collect(self._cfg)
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        lines = []
        # Plaid
        if snap.plaid_status or snap.plaid_last_sync:
            status_color = "green" if (snap.plaid_status or "").lower() in {"ok", "healthy", "success"} else "yellow"
            lines.append(
                f"[bold]plaid[/]  [{status_color}]{snap.plaid_status or '?'}[/]  "
                f"last_sync={snap.plaid_last_sync or '?'}"
            )
        else:
            lines.append(f"[bold]plaid[/]  [dim]{snap.plaid_note or 'n/a'}[/]")
        # DB counts
        if snap.db_counts:
            for c in snap.db_counts:
                if c.value is None:
                    lines.append(f"[bold]{c.label}[/]  [dim]{c.error or 'n/a'}[/]")
                else:
                    lines.append(f"[bold]{c.label}[/]  {c.value}")
        else:
            lines.append(f"[bold]db[/]     [dim]{snap.db_note or 'n/a'}[/]")
        lines.append("")
        # Log tail
        lines.append(f"[bold]log[/] ({snap.log_source or 'n/a'})")
        if snap.log_lines:
            for ln in snap.log_lines[-10:]:
                # truncate aggressively for panel width
                lines.append(f"  {ln[:120]}")
        elif snap.log_note:
            lines.append(f"  [dim]{snap.log_note}[/]")
        else:
            lines.append("  [dim](none)[/]")
        self.set_body("\n".join(lines))
        self.set_footer(snap.collected_at, snap.duration_ms)
