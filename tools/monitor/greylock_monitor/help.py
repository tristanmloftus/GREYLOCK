"""Help modal overlay."""

from __future__ import annotations

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import Static

HELP_TEXT = """\
[b]greylock-monitor[/b]  —  read-only TUI ops monitor for the Greylock server

[b]Keys[/b]
  q          quit
  r          force-refresh all panels
  ?          toggle this help
  esc        close this help

[b]Panels[/b]
  System     hostname, uptime, load, CPU, RAM, disk, swap          (psutil + /proc)
  Network    listening ports, egress/public IP, throughput          (ss, psutil, socket)
  Sessions   ssh users, tmux sessions, established TCP groups       (who, tmux, psutil)
  Greylock   app log tail, plaid status, db counts                  (file/journalctl/sqlite/pg)
  Auth       auth.log tail, fail2ban status                         (file/journalctl, fail2ban-client)
  Top Procs  top 8 by CPU and by RAM                                (psutil)

[b]Read-only guarantee[/b]
  no writes, no kills, no config changes. all DB handles opened read-only.
  the only outbound network call possible is the optional --public-ip fetch.

[b]Config[/b]
  ~/.config/greylock-monitor/config.toml   (override with --config PATH)
  see examples/config.toml.example in the repo.

[b]Permissions[/b]
  add the monitor user to the `adm` group for auth.log access.
  some metrics (process names on listening ports, full TCP visibility) show
  more detail when run as root, but root is NOT required.
"""


class HelpScreen(ModalScreen):
    BINDINGS = [
        Binding("escape", "dismiss_screen", "Close"),
        Binding("question_mark", "dismiss_screen", "Close"),
    ]

    DEFAULT_CSS = """
    HelpScreen {
        align: center middle;
        background: $background 70%;
    }
    HelpScreen > Vertical {
        width: 90;
        max-width: 90%;
        height: auto;
        max-height: 90%;
        border: round $accent;
        background: $surface;
        padding: 1 2;
    }
    """

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(HELP_TEXT, markup=True)

    def action_dismiss_screen(self) -> None:
        self.dismiss()
