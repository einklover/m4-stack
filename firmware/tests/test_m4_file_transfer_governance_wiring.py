#!/usr/bin/env python3
"""P1E RED/GREEN source contracts for file-transfer memory governance wiring."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_HEADER = ROOT / "src" / "network" / "M4FileTransferService.h"
SERVICE_SOURCE = ROOT / "src" / "network" / "M4FileTransferService.cpp"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_file_transfer_service_owns_governance_account():
    """Production ownership must expose one explicit service-owned governance boundary."""
    header = _read(SERVICE_HEADER)
    assert 'memory/M4FileTransferMemoryAccount.h' in header, (
        "file-transfer service does not include the P1E governance account"
    )
    assert "M4FileTransferMemoryAccount memoryAccount" in header, (
        "file-transfer service has no service-owned governance account"
    )


def test_http_runtime_admission_and_release_are_governed():
    """HTTP runtime admission, rollback, and stop must pass through governance."""
    source = _read(SERVICE_SOURCE)
    assert "memoryAccount.acquireHttpRuntime()" in source, (
        "HTTP runtime allocation can start without governed admission"
    )
    assert "memoryAccount.releaseHttpRuntime()" in source, (
        "HTTP runtime stop/rollback does not release governed ownership"
    )


def test_captive_dns_admission_and_release_are_governed():
    """Captive-DNS ownership must be accounted independently and released on stop."""
    source = _read(SERVICE_SOURCE)
    assert "memoryAccount.acquireDns()" in source, (
        "captive DNS allocation can occur without governed admission"
    )
    assert "memoryAccount.releaseDns()" in source, (
        "captive DNS cleanup does not release governed ownership"
    )
