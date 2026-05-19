"""Textual App — 3x2 grid of panels with per-panel refresh intervals."""

from __future__ import annotations

from textual.app import App, ComposeResult
from textual.containers import Grid
from textual.widgets import Footer, Header

from .collectors import processes as proc_collector
from .config import Config
from .help import HelpScreen
from .widgets.auth_panel import AuthPanel
from .widgets.greylock_panel import GreylockPanel
from .widgets.network_panel import NetworkPanel
from .widgets.processes_panel import ProcessesPanel
from .widgets.sessions_panel import SessionsPanel
from .widgets.system_panel import SystemPanel


class GreylockMonitorApp(App):
    CSS = """
    Grid#panels {
        grid-size: 3 2;
        grid-gutter: 0;
        padding: 0;
    }
    """

    BINDINGS = [
        ("q", "quit", "Quit"),
        ("r", "force_refresh", "Refresh"),
        ("question_mark", "toggle_help", "Help"),
        ("?", "toggle_help", "Help"),
    ]

    def __init__(self, *, enable_public_ip: bool = False, config: Config | None = None) -> None:
        super().__init__()
        self.enable_public_ip = enable_public_ip
        self.cfg = config or Config()
        self.system_panel = SystemPanel()
        self.network_panel = NetworkPanel(enable_public_ip=enable_public_ip)
        self.sessions_panel = SessionsPanel()
        self.greylock_panel = GreylockPanel(self.cfg.greylock)
        self.auth_panel = AuthPanel()
        self.processes_panel = ProcessesPanel()

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Grid(id="panels"):
            yield self.system_panel
            yield self.network_panel
            yield self.sessions_panel
            yield self.greylock_panel
            yield self.auth_panel
            yield self.processes_panel
        yield Footer()

    async def on_mount(self) -> None:
        proc_collector.seed()
        await self._refresh_all()
        self.set_interval(2.0, self._tick_2s)
        self.set_interval(5.0, self._tick_5s)

    async def _tick_2s(self) -> None:
        await self.system_panel.refresh_data()
        await self.network_panel.refresh_data()
        await self.processes_panel.refresh_data()

    async def _tick_5s(self) -> None:
        await self.sessions_panel.refresh_data()
        await self.auth_panel.refresh_data()
        await self.greylock_panel.refresh_data()

    async def _refresh_all(self) -> None:
        await self._tick_2s()
        await self._tick_5s()

    async def action_force_refresh(self) -> None:
        await self._refresh_all()

    def action_toggle_help(self) -> None:
        if isinstance(self.screen, HelpScreen):
            self.pop_screen()
        else:
            self.push_screen(HelpScreen())
