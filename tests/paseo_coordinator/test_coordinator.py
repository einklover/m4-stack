import json
import tempfile
import unittest
from pathlib import Path

from tools.paseo_coordinator.coordinator import Coordinator
from tools.paseo_coordinator.ledger import JsonlEventLedger
from tools.paseo_coordinator.model import ActionType, Event, EventType, TaskState
from tools.paseo_coordinator.state_machine import InvalidTransition, reduce_event


TASK_ID = "m4-68"


def make_event(event_type: EventType, **payload: object) -> Event:
    return Event(task_id=TASK_ID, type=event_type, payload=payload)


def advance(snapshot, *event_types: EventType):
    transition = None
    for event_type in event_types:
        transition = reduce_event(snapshot, make_event(event_type))
        snapshot = transition.snapshot
    return transition


class StateMachineTests(unittest.TestCase):
    def test_happy_path_reaches_completed_with_explicit_actions(self):
        snapshot = None

        transition = reduce_event(snapshot, make_event(EventType.TASK_CREATED))
        snapshot = transition.snapshot
        self.assertEqual(TaskState.CREATED, snapshot.state)

        transition = reduce_event(snapshot, make_event(EventType.ASSIGNED))
        snapshot = transition.snapshot
        self.assertEqual((ActionType.DISPATCH_EXECUTOR,), tuple(a.type for a in transition.actions))

        transition = reduce_event(snapshot, make_event(EventType.STARTED))
        snapshot = transition.snapshot
        transition = reduce_event(snapshot, make_event(EventType.RESULT))
        snapshot = transition.snapshot
        self.assertEqual(TaskState.RESULT_READY, snapshot.state)
        self.assertEqual((ActionType.REQUEST_REVIEW,), tuple(a.type for a in transition.actions))

        transition = reduce_event(snapshot, make_event(EventType.REVIEW_REQUESTED))
        snapshot = transition.snapshot
        transition = reduce_event(snapshot, make_event(EventType.REVIEW_PASSED))
        snapshot = transition.snapshot
        self.assertEqual(TaskState.REVIEW_PASSED, snapshot.state)
        self.assertEqual((ActionType.WAIT_FOR_CI,), tuple(a.type for a in transition.actions))

        transition = reduce_event(snapshot, make_event(EventType.CI_STARTED))
        snapshot = transition.snapshot
        transition = reduce_event(snapshot, make_event(EventType.CI_PASSED))
        self.assertEqual(TaskState.COMPLETED, transition.snapshot.state)
        self.assertEqual((ActionType.COMPLETE_TASK,), tuple(a.type for a in transition.actions))

    def test_invalid_transition_is_rejected(self):
        created = reduce_event(None, make_event(EventType.TASK_CREATED)).snapshot
        with self.assertRaises(InvalidTransition):
            reduce_event(created, make_event(EventType.CI_PASSED))

    def test_review_failure_returns_to_rework_and_dispatches_executor(self):
        transition = advance(
            None,
            EventType.TASK_CREATED,
            EventType.ASSIGNED,
            EventType.STARTED,
            EventType.RESULT,
            EventType.REVIEW_REQUESTED,
        )
        failed = reduce_event(transition.snapshot, make_event(EventType.REVIEW_FAILED))
        self.assertEqual(TaskState.REWORK, failed.snapshot.state)
        self.assertEqual((ActionType.DISPATCH_EXECUTOR,), tuple(a.type for a in failed.actions))

    def test_ci_failure_returns_to_rework_and_dispatches_executor(self):
        transition = advance(
            None,
            EventType.TASK_CREATED,
            EventType.ASSIGNED,
            EventType.STARTED,
            EventType.RESULT,
            EventType.REVIEW_REQUESTED,
            EventType.REVIEW_PASSED,
            EventType.CI_STARTED,
        )
        failed = reduce_event(transition.snapshot, make_event(EventType.CI_FAILED))
        self.assertEqual(TaskState.REWORK, failed.snapshot.state)
        self.assertEqual((ActionType.DISPATCH_EXECUTOR,), tuple(a.type for a in failed.actions))

    def test_pause_resume_restores_running_and_emits_continuation_action(self):
        running = advance(
            None,
            EventType.TASK_CREATED,
            EventType.ASSIGNED,
            EventType.STARTED,
        ).snapshot

        paused = reduce_event(running, make_event(EventType.USER_PAUSED))
        self.assertEqual(TaskState.PAUSED, paused.snapshot.state)
        self.assertEqual(TaskState.RUNNING, paused.snapshot.resume_state)
        self.assertEqual((ActionType.CANCEL_AGENT,), tuple(a.type for a in paused.actions))

        resumed = reduce_event(paused.snapshot, make_event(EventType.USER_RESUMED))
        self.assertEqual(TaskState.RUNNING, resumed.snapshot.state)
        self.assertIsNone(resumed.snapshot.resume_state)
        self.assertEqual((ActionType.DISPATCH_EXECUTOR,), tuple(a.type for a in resumed.actions))


class LedgerCoordinatorTests(unittest.TestCase):
    def test_invalid_event_is_not_appended_and_replay_reproduces_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            ledger = JsonlEventLedger(path)
            coordinator = Coordinator(ledger)

            transition = coordinator.apply(None, make_event(EventType.TASK_CREATED))
            transition = coordinator.apply(transition.snapshot, make_event(EventType.ASSIGNED, agent="luna"))

            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(2, len(lines))
            self.assertEqual(
                {"task_id": TASK_ID, "type": "TASK_CREATED", "payload": {}},
                json.loads(lines[0]),
            )

            with self.assertRaises(InvalidTransition):
                coordinator.apply(transition.snapshot, make_event(EventType.CI_PASSED))

            self.assertEqual(2, len(path.read_text(encoding="utf-8").splitlines()))
            replayed = coordinator.replay(TASK_ID)
            self.assertIsNotNone(replayed)
            self.assertEqual(TaskState.ASSIGNED, replayed.state)
            self.assertEqual(TASK_ID, replayed.task_id)


if __name__ == "__main__":
    unittest.main()
