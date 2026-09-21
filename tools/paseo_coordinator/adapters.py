from __future__ import annotations

from typing import Mapping, Protocol, Sequence

from .model import Action, Event, TaskSnapshot


class PaseoAdapter(Protocol):
    def dispatch_executor(self, task_id: str, payload: Mapping[str, object]) -> None:
        ...

    def cancel_agent(self, task_id: str, payload: Mapping[str, object]) -> None:
        ...


class SlackAdapter(Protocol):
    def publish(
        self,
        task_id: str,
        event: Event,
        snapshot: TaskSnapshot,
        actions: Sequence[Action],
    ) -> None:
        ...


class GitHubAdapter(Protocol):
    def observe_ci(self, task_id: str, payload: Mapping[str, object]) -> None:
        ...
