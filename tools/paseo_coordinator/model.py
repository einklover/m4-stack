from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class TaskState(str, Enum):
    CREATED = "CREATED"
    ASSIGNED = "ASSIGNED"
    RUNNING = "RUNNING"
    RESULT_READY = "RESULT_READY"
    AWAITING_REVIEW = "AWAITING_REVIEW"
    REWORK = "REWORK"
    REVIEW_PASSED = "REVIEW_PASSED"
    AWAITING_CI = "AWAITING_CI"
    PAUSED = "PAUSED"
    COMPLETED = "COMPLETED"


class EventType(str, Enum):
    TASK_CREATED = "TASK_CREATED"
    ASSIGNED = "ASSIGNED"
    STARTED = "STARTED"
    RESULT = "RESULT"
    REVIEW_REQUESTED = "REVIEW_REQUESTED"
    REVIEW_FAILED = "REVIEW_FAILED"
    REVIEW_PASSED = "REVIEW_PASSED"
    CI_STARTED = "CI_STARTED"
    CI_FAILED = "CI_FAILED"
    CI_PASSED = "CI_PASSED"
    USER_PAUSED = "USER_PAUSED"
    USER_RESUMED = "USER_RESUMED"


class ActionType(str, Enum):
    DISPATCH_EXECUTOR = "DISPATCH_EXECUTOR"
    REQUEST_REVIEW = "REQUEST_REVIEW"
    WAIT_FOR_CI = "WAIT_FOR_CI"
    CANCEL_AGENT = "CANCEL_AGENT"
    COMPLETE_TASK = "COMPLETE_TASK"


@dataclass(frozen=True, slots=True)
class Event:
    task_id: str
    type: EventType
    payload: dict[str, object] = field(default_factory=dict)


@dataclass(frozen=True, slots=True)
class Action:
    type: ActionType
    payload: dict[str, object] = field(default_factory=dict)


@dataclass(frozen=True, slots=True)
class TaskSnapshot:
    task_id: str
    state: TaskState
    resume_state: TaskState | None = None


@dataclass(frozen=True, slots=True)
class Transition:
    snapshot: TaskSnapshot
    actions: tuple[Action, ...] = ()
