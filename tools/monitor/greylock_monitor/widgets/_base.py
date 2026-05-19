"""Shared widget primitives."""

from __future__ import annotations

import time
from datetime import datetime

from textual.containers import Vertical
from textual.widgets import Static


class Panel(Vertical):
    """Bordered panel with title and a footer line showing freshness."""

    DEFAULT_CSS = """
    Panel {
        border: round $panel-lighten-2;
        padding: 0 1;
    }
    Panel > .panel-body {
        height: 1fr;
    }
    Panel > .panel-footer {
        height: 1;
        color: $text-muted;
        text-style: dim;
    }
    """

    def __init__(self, title: str, **kwargs) -> None:
        super().__init__(**kwargs)
        self.border_title = title
        self._body = Static("", classes="panel-body", markup=True)
        self._footer = Static("", classes="panel-footer", markup=True)

    def compose(self):
        yield self._body
        yield self._footer

    def set_body(self, content: str) -> None:
        self._body.update(content)

    def set_footer(self, collected_at: float | None, duration_ms: float | None, note: str = "") -> None:
        if collected_at is None:
            self._footer.update(note or "—")
            return
        ts = datetime.fromtimestamp(collected_at).strftime("%H:%M:%S")
        age = max(0.0, time.time() - collected_at)
        stale = " · [yellow]stale[/]" if age > 10 else ""
        dur = f" · {int(duration_ms)}ms" if duration_ms is not None else ""
        suffix = f" · {note}" if note else ""
        self._footer.update(f"updated {ts}{dur}{stale}{suffix}")

    def set_error(self, reason: str) -> None:
        self._body.update(f"[red]panel error:[/] {reason}")
