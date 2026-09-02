#!/usr/bin/env python3
"""P2 RED/GREEN contracts: simulator-only memory governance invariants."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEMORY_DIR = ROOT / "src" / "memory"


def test_memory_governance_contract_marker_exists():
    """Verify the deterministic memory governance implementation exists."""
    marker = MEMORY_DIR / "M4MemoryGovernance.cpp"
    assert marker.exists(), "missing M4MemoryGovernance implementation"


def test_qemu_memory_budget_contract():
    """Verify the QEMU/host memory governance contract header exists."""
    budget_header = MEMORY_DIR / "M4MemoryGovernance.h"
    assert budget_header.exists(), "missing deterministic memory budget contract header"
