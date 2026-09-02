#!/usr/bin/env python3
"""P2 RED contracts: simulator-only memory governance invariants.

This intentionally fails until the runtime memory ownership implementation
provides deterministic accounting hooks.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_memory_governance_contract_marker_exists():
    """RED: implementation marker is not present before P2 GREEN work."""
    marker = ROOT / "src" / "util" / "M4MemoryGovernance.cpp"
    assert marker.exists(), (
        "P2 RED: expected M4MemoryGovernance implementation contract marker "
        "to be added by GREEN migration"
    )


def test_qemu_memory_budget_contract():
    """RED: QEMU budget enforcement hook must be introduced."""
    budget_header = ROOT / "src" / "util" / "M4MemoryGovernance.h"
    assert budget_header.exists(), (
        "P2 RED: missing deterministic memory budget contract header"
    )
