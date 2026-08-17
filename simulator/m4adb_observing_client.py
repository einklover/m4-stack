"""m4sim client variant that never drops serial lines sharing a reply read.

The production m4adb Client currently returns from inside the parser loop as
soon as it sees the matching OK/ERR frame.  If a firmware lifecycle log follows
that frame in the same OS read, the parser has already consumed it but the
caller never records it.  Simulator E2E tests rely on those ordinary serial
lines, so drain the complete parsed batch before returning the matched reply.

Keep this subclass deliberately small: protocol framing, transport, request IDs
and all high-level Client methods remain the production implementation.
"""
from __future__ import annotations

import time
from typing import Any, Callable, Optional

from m4adb_lib.client import BridgeError, Client
from m4adb_lib.protocol import build_req


class ObservingClient(Client):
    """Client.request() with lossless logging for the current read batch."""

    def close(self) -> None:
        """Close the underlying serial transport used by simulator journeys."""
        self.t.close()

    def request(
        self,
        obj: dict[str, Any],
        timeout: Optional[float] = None,
        req_id: Optional[str] = None,
        on_progress: Optional[Callable[[dict[str, Any]], None]] = None,
    ) -> dict:
        rid = req_id or self._next_id()
        self.t.write(build_req(rid, obj))
        timeout = self.default_timeout if timeout is None else timeout
        timeout_f = float(timeout)
        started_at = time.time()
        deadline = started_at + timeout_f
        last_progress_at = started_at
        last_heartbeat = started_at

        while time.time() < deadline:
            data = self.t.read(timeout=0.1)
            if not data:
                if on_progress and time.time() - last_heartbeat >= 5.0:
                    now = time.time()
                    on_progress(
                        {
                            "op": obj.get("op"),
                            "phase": "host_wait",
                            "pct": -1,
                            "waited_s": round(now - started_at, 1),
                            "remain_s": round(max(0.0, deadline - now), 1),
                        }
                    )
                    last_heartbeat = now
                continue

            matched_result: dict | None = None
            matched_error: BridgeError | None = None
            matched = False

            # Important: process every item from this parser.feed(data) batch.
            # A matching reply may be followed by ordinary firmware log lines
            # that have already been removed from the serial driver's RX queue.
            for raw_line, frame in self.parser.feed(data):
                if frame is None:
                    self._emit_log(raw_line)
                    continue
                self._emit_log(raw_line)
                if frame.req_id != rid:
                    continue
                if frame.kind == "prg":
                    last_progress_at = time.time()
                    last_heartbeat = time.time()
                    if on_progress:
                        on_progress(frame.json or {})
                    continue
                if frame.kind == "err" and not matched:
                    j = frame.json or {}
                    matched_error = BridgeError(j.get("error", "error"), j.get("message", ""))
                    matched = True
                    continue
                if frame.kind == "ok" and not matched:
                    matched_result = frame.json or {}
                    matched = True

            if matched_error is not None:
                raise matched_error
            if matched:
                return matched_result or {}

            # Preserve production Client's progress bookkeeping semantics.
            _ = last_progress_at

        raise BridgeError("timeout", f"等待响应超时 ({timeout}s)")
