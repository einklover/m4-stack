from __future__ import annotations

from .ledger import JsonlEventLedger
from .model import Event, TaskSnapshot, Transition
from .state_machine import reduce_event


class Coordinator:
    def __init__(self, ledger: JsonlEventLedger):
        self.ledger = ledger

    def apply(self, snapshot: TaskSnapshot | None, event: Event) -> Transition:
        transition = reduce_event(snapshot, event)
        self.ledger.append(event)
        return transition

    def replay(self, task_id: str) -> TaskSnapshot | None:
        snapshot: TaskSnapshot | None = None
        for event in self.ledger.read_all():
            if event.task_id != task_id:
                continue
            snapshot = reduce_event(snapshot, event).snapshot
        return snapshot
