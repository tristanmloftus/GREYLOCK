"""Auth/security panel widget."""

from __future__ import annotations

from ..collectors import auth as auth_collector
from ._base import Panel


def _color_for(kind: str) -> str:
    return {"ok": "green", "fail": "red", "info": "dim"}.get(kind, "white")


class AuthPanel(Panel):
    def __init__(self) -> None:
        super().__init__("Auth")

    async def refresh_data(self) -> None:
        try:
            snap = await auth_collector.collect()
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        lines = []
        lines.append("[bold]ssh auth (last 10)[/]")
        if snap.lines:
            for line in snap.lines:
                color = _color_for(line.kind)
                lines.append(f"  [{color}]{line.raw[:140]}[/]")
        elif snap.lines_note:
            lines.append(f"  [dim]{snap.lines_note}[/]")
        else:
            lines.append("  [dim](none)[/]")
        if snap.fail2ban_available:
            lines.append("")
            lines.append("[bold]fail2ban[/]")
            if snap.fail2ban:
                for j in snap.fail2ban:
                    lines.append(f"  {j.name:<10} cur={j.currently_banned}  total={j.total_banned}")
            elif snap.fail2ban_note:
                lines.append(f"  [dim]{snap.fail2ban_note}[/]")
            else:
                lines.append("  [dim](no jails)[/]")
        self.set_body("\n".join(lines))
        self.set_footer(snap.collected_at, snap.duration_ms)
