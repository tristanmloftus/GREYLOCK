"""Smoke tests — every collector returns its dataclass without raising,
even on a system where most data sources are absent."""

from __future__ import annotations

import pytest

from greylock_monitor.collectors import (
    auth,
    greylock,
    network,
    processes,
    sessions,
    system,
)
from greylock_monitor.config import Config, GreylockConfig


def test_system_collect_returns_snapshot():
    snap = system.collect()
    assert isinstance(snap, system.SystemSnapshot)
    assert snap.hostname  # always present
    assert snap.error is None
    assert snap.duration_ms >= 0


def test_processes_collect_returns_snapshot():
    processes.seed()
    snap = processes.collect(limit=5)
    assert isinstance(snap, processes.ProcessesSnapshot)
    assert snap.error is None
    # On any real system we expect at least one process visible.
    assert len(snap.top_cpu) > 0
    assert all(isinstance(r, processes.ProcInfo) for r in snap.top_cpu)


@pytest.mark.asyncio
async def test_network_collect_returns_snapshot():
    nc = network.NetworkCollector()
    snap = await nc.collect(enable_public_ip=False)
    assert isinstance(snap, network.NetworkSnapshot)
    assert snap.error is None
    # First call seeds throughput baseline; both should be None.
    assert snap.bytes_recv_per_s is None
    assert snap.bytes_sent_per_s is None


@pytest.mark.asyncio
async def test_sessions_collect_returns_snapshot():
    snap = await sessions.collect()
    assert isinstance(snap, sessions.SessionsSnapshot)
    assert snap.error is None


@pytest.mark.asyncio
async def test_greylock_collect_degrades_without_config():
    snap = await greylock.collect(GreylockConfig())
    assert isinstance(snap, greylock.GreylockSnapshot)
    assert snap.error is None
    # No DB configured → expect db_note explanatory string.
    assert snap.db_counts == []
    assert snap.db_note is not None


@pytest.mark.asyncio
async def test_auth_collect_returns_snapshot():
    snap = await auth.collect()
    assert isinstance(snap, auth.AuthSnapshot)
    assert snap.error is None


def test_config_load_empty():
    cfg = Config()
    assert cfg.greylock.log_path is None
    assert cfg.greylock.db is None
    assert cfg.has_greylock_section is False
