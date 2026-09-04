from __future__ import annotations

from .model import Action, ActionType, Event, EventType, TaskSnapshot, TaskState, Transition


class InvalidTransition(ValueError):
    pass


def _action(action_type: ActionType, event: Event) -> Action:
    return Action(type=action_type, payload=dict(event.payload))


def _resume_actions(state: TaskState, event: Event) -> tuple[Action, ...]:
    if state in {TaskState.ASSIGNED, TaskState.RUNNING, TaskState.REWORK}:
        return (_action(ActionType.DISPATCH_EXECUTOR, event),)
    if state in {TaskState.RESULT_READY, TaskState.AWAITING_REVIEW}:
        return (_action(ActionType.REQUEST_REVIEW, event),)
    if state in {TaskState.REVIEW_PASSED, TaskState.AWAITING_CI}:
        return (_action(ActionType.WAIT_FOR_CI, event),)
    return ()


def _next(snapshot: TaskSnapshot, state: TaskState, *actions: Action) -> Transition:
    return Transition(
        snapshot=TaskSnapshot(task_id=snapshot.task_id, state=state),
        actions=tuple(actions),
    )


def reduce_event(snapshot: TaskSnapshot | None, event: Event) -> Transition:
    if not event.task_id:
        raise InvalidTransition("task_id must not be empty")

    if snapshot is None:
        if event.type is not EventType.TASK_CREATED:
            raise InvalidTransition(f"first event must be TASK_CREATED, got {event.type.value}")
        return Transition(TaskSnapshot(task_id=event.task_id, state=TaskState.CREATED))

    if snapshot.task_id != event.task_id:
        raise InvalidTransition(
            f"event task_id {event.task_id!r} does not match snapshot {snapshot.task_id!r}"
        )

    if event.type is EventType.USER_PAUSED:
        if snapshot.state in {TaskState.PAUSED, TaskState.COMPLETED}:
            raise InvalidTransition(f"cannot pause task in {snapshot.state.value}")
        return Transition(
            snapshot=TaskSnapshot(
                task_id=snapshot.task_id,
                state=TaskState.PAUSED,
                resume_state=snapshot.state,
            ),
            actions=(_action(ActionType.CANCEL_AGENT, event),),
        )

    if event.type is EventType.USER_RESUMED:
        if snapshot.state is not TaskState.PAUSED or snapshot.resume_state is None:
            raise InvalidTransition(f"cannot resume task in {snapshot.state.value}")
        restored = snapshot.resume_state
        return Transition(
            snapshot=TaskSnapshot(task_id=snapshot.task_id, state=restored),
            actions=_resume_actions(restored, event),
        )

    state = snapshot.state

    if state is TaskState.CREATED and event.type is EventType.ASSIGNED:
        return _next(snapshot, TaskState.ASSIGNED, _action(ActionType.DISPATCH_EXECUTOR, event))

    if state in {TaskState.ASSIGNED, TaskState.REWORK} and event.type is EventType.STARTED:
        return _next(snapshot, TaskState.RUNNING)

    if state is TaskState.RUNNING and event.type is EventType.RESULT:
        return _next(snapshot, TaskState.RESULT_READY, _action(ActionType.REQUEST_REVIEW, event))

    if state is TaskState.RESULT_READY and event.type is EventType.REVIEW_REQUESTED:
        return _next(snapshot, TaskState.AWAITING_REVIEW)

    if state is TaskState.AWAITING_REVIEW and event.type is EventType.REVIEW_FAILED:
        return _next(snapshot, TaskState.REWORK, _action(ActionType.DISPATCH_EXECUTOR, event))

    if state is TaskState.AWAITING_REVIEW and event.type is EventType.REVIEW_PASSED:
        return _next(snapshot, TaskState.REVIEW_PASSED, _action(ActionType.WAIT_FOR_CI, event))

    if state is TaskState.REVIEW_PASSED and event.type is EventType.CI_STARTED:
        return _next(snapshot, TaskState.AWAITING_CI)

    if state is TaskState.AWAITING_CI and event.type is EventType.CI_FAILED:
        return _next(snapshot, TaskState.REWORK, _action(ActionType.DISPATCH_EXECUTOR, event))

    if state is TaskState.AWAITING_CI and event.type is EventType.CI_PASSED:
        return _next(snapshot, TaskState.COMPLETED, _action(ActionType.COMPLETE_TASK, event))

    raise InvalidTransition(f"event {event.type.value} is invalid while task is {state.value}")
