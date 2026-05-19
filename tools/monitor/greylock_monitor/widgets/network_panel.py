"""Network panel widget."""

from __future__ import annotations

from ..collectors.network import NetworkCollector, format_bytes_per_s
from ._base import Panel


class NetworkPanel(Panel):
    def __init__(self, *, enable_public_ip: bool = False) -> None:
        super().__init__("Network")
        self._collector = NetworkCollector()
        self._enable_public_ip = enable_public_ip

    async def refresh_data(self) -> None:
        try:
            snap = await self._collector.collect(enable_public_ip=self._enable_public_ip)
        except Exception as e:  # pragma: no cover
            self.set_error(f"{type(e).__name__}: {e}")
            return
        if snap.error:
            self.set_body(f"[red]n/a[/] — {snap.error}")
            self.set_footer(snap.collected_at, snap.duration_ms)
            return
        lines = []
        egress = snap.egress_ip or "n/a"
        public = snap.public_ip or ("[dim]disabled[/]" if not self._enable_public_ip else "n/a")
        lines.append(f"[bold]egress[/]  {egress}")
        lines.append(f"[bold]public[/]  {public}")
        lines.append(
            f"[bold]net[/]     [green]↓ {format_bytes_per_s(snap.bytes_recv_per_s)}[/]  "
            f"[blue]↑ {format_bytes_per_s(snap.bytes_sent_per_s)}[/]"
        )
        lines.append("")
        lines.append("[bold]listening[/]")
        if not snap.listening:
            lines.append("[dim](none)[/]")
        else:
            for p in snap.listening[:12]:
                proc = p.process or "[dim]?[/]"
                lines.append(f"  {p.port:>5}/{p.proto}  {proc}")
            if len(snap.listening) > 12:
                lines.append(f"  [dim]+ {len(snap.listening) - 12} more[/]")
        self.set_body("\n".join(lines))
        self.set_footer(snap.collected_at, snap.duration_ms, snap.listening_note or "")
