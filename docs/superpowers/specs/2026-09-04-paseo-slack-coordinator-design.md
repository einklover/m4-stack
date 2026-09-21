# Paseo + Slack Thin Coordinator Design

**Date:** 2026-09-04
**Issue:** #68
**Status:** prototype design approved in chat

## Purpose

Add a small, deterministic coordination layer around the project's existing Paseo execution path. Slack remains the human-visible control room, Paseo remains the agent runtime, and GitHub remains the durable engineering evidence store. The coordinator must not become a second agent runtime and must not alter firmware/product behavior.

## Existing foundation to reuse

The repository already has `.github/workflows/paseo-subagent.yml`, which validates an authorized task marker on GitHub and dispatches it on the self-hosted macOS/ARM64/Paseo runner through the existing `paseo-agent-runner`. The prototype therefore does **not** implement a new Paseo launcher. It defines deterministic task state and adapter contracts that can drive the existing launcher.

## Architecture

The prototype has four layers:

1. **Pure state machine** — accepts one event and a prior snapshot, rejects invalid transitions, and returns the next snapshot plus explicit actions.
2. **Append-only event ledger** — JSON Lines persistence for replay/audit during the prototype. Invalid events are never appended.
3. **Coordinator** — combines reducer + ledger and supports replaying a task from its events.
4. **Adapters (interfaces only in this prototype)** — describe how an outer process can dispatch/cancel Paseo work, publish Slack updates, and observe GitHub/CI without embedding those side effects in the reducer.

The state engine contains no LLM calls. Agent prose cannot silently change task state.

## State model

`TaskState` values:

- `CREATED`
- `ASSIGNED`
- `RUNNING`
- `RESULT_READY`
- `AWAITING_REVIEW`
- `REWORK`
- `REVIEW_PASSED`
- `AWAITING_CI`
- `PAUSED`
- `COMPLETED`

`PAUSED` stores the exact prior state in `resume_state`. `COMPLETED` is terminal.

## Event model

`EventType` values:

- `TASK_CREATED`
- `ASSIGNED`
- `STARTED`
- `RESULT`
- `REVIEW_REQUESTED`
- `REVIEW_FAILED`
- `REVIEW_PASSED`
- `CI_STARTED`
- `CI_FAILED`
- `CI_PASSED`
- `USER_PAUSED`
- `USER_RESUMED`

Each event includes `task_id` plus a JSON-serializable payload. The reducer does not create timestamps; an outer adapter may include timestamps in payload metadata if desired.

## Explicit actions

`ActionType` values:

- `DISPATCH_EXECUTOR`
- `REQUEST_REVIEW`
- `WAIT_FOR_CI`
- `CANCEL_AGENT`
- `COMPLETE_TASK`

Actions are instructions to adapters, not side effects performed by the state machine.

## Transition contract

Normal path:

1. no snapshot + `TASK_CREATED` -> `CREATED`
2. `CREATED` + `ASSIGNED` -> `ASSIGNED`, action `DISPATCH_EXECUTOR`
3. `ASSIGNED` or `REWORK` + `STARTED` -> `RUNNING`
4. `RUNNING` + `RESULT` -> `RESULT_READY`, action `REQUEST_REVIEW`
5. `RESULT_READY` + `REVIEW_REQUESTED` -> `AWAITING_REVIEW`
6. `AWAITING_REVIEW` + `REVIEW_FAILED` -> `REWORK`, action `DISPATCH_EXECUTOR`
7. `AWAITING_REVIEW` + `REVIEW_PASSED` -> `REVIEW_PASSED`, action `WAIT_FOR_CI`
8. `REVIEW_PASSED` + `CI_STARTED` -> `AWAITING_CI`
9. `AWAITING_CI` + `CI_FAILED` -> `REWORK`, action `DISPATCH_EXECUTOR`
10. `AWAITING_CI` + `CI_PASSED` -> `COMPLETED`, action `COMPLETE_TASK`

Pause/resume:

- Any non-terminal, non-paused state + `USER_PAUSED` -> `PAUSED`, preserve prior state in `resume_state`, action `CANCEL_AGENT`.
- `PAUSED` + `USER_RESUMED` restores `resume_state` and emits the action needed to continue safely:
  - `ASSIGNED`, `RUNNING`, `REWORK` -> `DISPATCH_EXECUTOR`
  - `RESULT_READY`, `AWAITING_REVIEW` -> `REQUEST_REVIEW`
  - `REVIEW_PASSED`, `AWAITING_CI` -> `WAIT_FOR_CI`
  - `CREATED` -> no action.

All other state/event combinations raise `InvalidTransition`.

## Persistence and replay

The ledger stores one JSON object per line using enum string values. `append()` opens the file in append mode, writes exactly one line, flushes it, and calls `fsync` before returning. `read_all()` reconstructs `Event` objects. `Coordinator.replay(task_id)` deterministically folds the matching events from an empty snapshot.

The coordinator first calculates a transition and only appends the event if the transition is valid. This prevents invalid input from polluting the audit trail.

## Adapter boundary

`tools/paseo_coordinator/adapters.py` defines `Protocol` interfaces only:

- `PaseoAdapter.dispatch_executor(task_id, payload)` and `cancel_agent(task_id, payload)`
- `SlackAdapter.publish(task_id, event, snapshot, actions)`
- `GitHubAdapter.observe_ci(task_id, payload)`

A later integration can map `DISPATCH_EXECUTOR` to the existing `[PASEO_TASK v1]` / `paseo-agent-runner` path, map `CANCEL_AGENT` to Paseo cancellation, and map state changes to one Slack thread. Those integrations must not modify reducer semantics.

## Repository boundaries

All production code lives under `tools/paseo_coordinator/`; tests live under `tests/paseo_coordinator/`; the dedicated prototype workflow lives at `.github/workflows/paseo-coordinator.yml`. No file under `firmware/`, `simulator/`, `plugins/`, or product UI is changed.

## Verification

A dedicated Python-stdlib-only GitHub Actions job runs:

```bash
python3 -m unittest discover -s tests/paseo_coordinator -v
```

Required tests cover:

- happy path to `COMPLETED`
- invalid transition rejection
- review-failure rework loop
- CI-failure rework loop
- pause/resume restoring the prior phase and emitting the correct continuation action
- append-only ledger persistence and replay

The implementation has no third-party Python dependency.
