from __future__ import annotations

import json
import os
from pathlib import Path

from .model import Event, EventType


class JsonlEventLedger:
    def __init__(self, path: str | Path):
        self.path = Path(path)

    def append(self, event: Event) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "task_id": event.task_id,
            "type": event.type.value,
            "payload": event.payload,
        }
        encoded = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(encoded)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())

    def read_all(self) -> list[Event]:
        if not self.path.exists():
            return []

        events: list[Event] = []
        with self.path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                text = line.strip()
                if not text:
                    continue
                raw = json.loads(text)
                payload = raw.get("payload", {})
                if not isinstance(payload, dict):
                    raise ValueError(f"ledger line {line_number} payload must be an object")
                events.append(
                    Event(
                        task_id=str(raw["task_id"]),
                        type=EventType(str(raw["type"])),
                        payload=payload,
                    )
                )
        return events
