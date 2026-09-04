# Paseo + Slack Thin Coordinator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a small deterministic Task/Event coordinator that wraps the repository's existing Paseo dispatch path and can later be controlled from Slack without adding another agent framework/runtime.

**Architecture:** A pure reducer owns all state transitions and emits explicit adapter actions. A JSONL ledger supplies append-only audit/replay, and a thin `Coordinator` combines the two. External Paseo/Slack/GitHub effects are represented by Protocol interfaces so the core is testable and cannot be steered implicitly by model prose.

**Tech Stack:** Python 3 standard library (`dataclasses`, `enum`, `json`, `pathlib`, `typing`, `os`), `unittest`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-04-paseo-slack-coordinator-design.md`

## Global Constraints

- Do not modify `firmware/`, `simulator/`, `plugins/`, hardware behavior, or product UI.
- Reuse the existing `.github/workflows/paseo-subagent.yml` / `paseo-agent-runner` execution path; do not create a second Paseo launcher.
- No CrewAI, LangGraph, AutoGen, Microsoft Agent Framework, Temporal, or other orchestration runtime dependency in the prototype.
- State transitions are deterministic and occur only through typed events.
- Invalid transitions must be rejected before ledger persistence.
- The JSONL ledger is append-only.
- Use Python standard library only.
- Test-first: establish a real RED GitHub Actions run before adding production code, then verify GREEN on the implementation tip.

---

### Task 1: Establish the RED contract and dedicated CI

**Files:**
- Create: `.github/workflows/paseo-coordinator.yml`
- Create: `tests/paseo_coordinator/test_coordinator.py`

**Interfaces:**
- Consumes: none.
- Produces: executable behavioral contract for `tools.paseo_coordinator` and a dedicated CI gate.

- [ ] **Step 1: Add the dedicated workflow**

```yaml
name: Paseo coordinator prototype

on:
  push:
    branches: [prototype/paseo-slack-coordinator]
    paths:
      - 'tools/paseo_coordinator/**'
      - 'tests/paseo_coordinator/**'
      - '.github/workflows/paseo-coordinator.yml'
  pull_request:
    paths:
      - 'tools/paseo_coordinator/**'
      - 'tests/paseo_coordinator/**'
      - '.github/workflows/paseo-coordinator.yml'

permissions:
  contents: read

jobs:
  unit:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.12'
      - run: python3 -m unittest discover -s tests/paseo_coordinator -v
```

- [ ] **Step 2: Add tests before production code**

The test module imports:

```python
from tools.paseo_coordinator.coordinator import Coordinator
from tools.paseo_coordinator.ledger import JsonlEventLedger
from tools.paseo_coordinator.model import ActionType, Event, EventType, TaskState
from tools.paseo_coordinator.state_machine import InvalidTransition, reduce_event
```

It contains independent tests for:

```python
# happy path: TASK_CREATED -> ... -> CI_PASSED == COMPLETED
# invalid transition: CREATED + CI_PASSED raises InvalidTransition
# review failure: AWAITING_REVIEW + REVIEW_FAILED == REWORK + DISPATCH_EXECUTOR
# CI failure: AWAITING_CI + CI_FAILED == REWORK + DISPATCH_EXECUTOR
# pause/resume: RUNNING -> PAUSED -> RUNNING; pause emits CANCEL_AGENT; resume emits DISPATCH_EXECUTOR
# ledger/coordinator: events append as JSONL, invalid event is not appended, replay reproduces the snapshot
```

- [ ] **Step 3: Push only workflow + tests and verify RED**

Expected failure: `ModuleNotFoundError: No module named 'tools.paseo_coordinator'` (or equivalent missing production module). A syntax/configuration failure does not count as RED; fix the test/workflow until the failure is specifically caused by the missing implementation.

- [ ] **Step 4: Record the RED run on Issue #68 / Draft PR**

Record the exact commit SHA and Actions run ID before any production code is added.

---

### Task 2: Implement typed model and deterministic reducer

**Files:**
- Create: `tools/paseo_coordinator/__init__.py`
- Create: `tools/paseo_coordinator/model.py`
- Create: `tools/paseo_coordinator/state_machine.py`
- Test: `tests/paseo_coordinator/test_coordinator.py`

**Interfaces:**
- Produces:
  - `TaskState(str, Enum)`
  - `EventType(str, Enum)`
  - `ActionType(str, Enum)`
  - `Event(task_id: str, type: EventType, payload: dict[str, object])`
  - `Action(type: ActionType, payload: dict[str, object])`
  - `TaskSnapshot(task_id: str, state: TaskState, resume_state: TaskState | None = None)`
  - `Transition(snapshot: TaskSnapshot, actions: tuple[Action, ...])`
  - `InvalidTransition(ValueError)`
  - `reduce_event(snapshot: TaskSnapshot | None, event: Event) -> Transition`

- [ ] **Step 1: Add minimal enums/dataclasses exactly matching the design spec**

`model.py` contains no I/O and no generated timestamps.

- [ ] **Step 2: Implement explicit transition table / branches**

The reducer must only accept the transitions in the design spec. It validates `event.task_id` against an existing snapshot before processing.

- [ ] **Step 3: Implement pause/resume**

On pause, set `state=PAUSED`, save the exact previous state in `resume_state`, and return `CANCEL_AGENT`. On resume, restore the previous state and emit its deterministic continuation action.

- [ ] **Step 4: Run the dedicated unit suite**

Run:

```bash
python3 -m unittest discover -s tests/paseo_coordinator -v
```

Expected: reducer/model tests pass; ledger/coordinator imports may still fail until Task 3 if tests are split by module. If the contract is in one module, implement Task 3 immediately before claiming GREEN.

---

### Task 3: Implement append-only ledger and coordinator replay

**Files:**
- Create: `tools/paseo_coordinator/ledger.py`
- Create: `tools/paseo_coordinator/coordinator.py`
- Test: `tests/paseo_coordinator/test_coordinator.py`

**Interfaces:**
- Consumes: `Event`, `TaskSnapshot`, `Transition`, `reduce_event` from Task 2.
- Produces:
  - `JsonlEventLedger(path: str | Path)`
  - `JsonlEventLedger.append(event: Event) -> None`
  - `JsonlEventLedger.read_all() -> list[Event]`
  - `Coordinator(ledger: JsonlEventLedger)`
  - `Coordinator.apply(snapshot: TaskSnapshot | None, event: Event) -> Transition`
  - `Coordinator.replay(task_id: str) -> TaskSnapshot | None`

- [ ] **Step 1: Implement JSONL encoding/decoding**

Each line has this stable shape:

```json
{"task_id":"m4-68","type":"TASK_CREATED","payload":{}}
```

`append()` opens the path with UTF-8 append mode, writes one compact JSON object plus newline, flushes, then `os.fsync()`s the file descriptor.

- [ ] **Step 2: Implement apply-before-append validation**

```python
transition = reduce_event(snapshot, event)
ledger.append(event)
return transition
```

If `reduce_event` raises, `ledger.append` is never called.

- [ ] **Step 3: Implement replay**

Fold only matching `task_id` events from `None`, applying `reduce_event` in ledger order. Do not append anything while replaying.

- [ ] **Step 4: Run all unit tests**

Expected: all coordinator prototype tests PASS.

---

### Task 4: Add adapter contracts for existing Paseo/Slack/GitHub integrations

**Files:**
- Create: `tools/paseo_coordinator/adapters.py`
- Modify: `tools/paseo_coordinator/__init__.py`
- Test: `tests/paseo_coordinator/test_coordinator.py` only if exports need coverage.

**Interfaces:**
- Consumes: `Event`, `TaskSnapshot`, `Action`.
- Produces Protocols:

```python
class PaseoAdapter(Protocol):
    def dispatch_executor(self, task_id: str, payload: Mapping[str, object]) -> None: ...
    def cancel_agent(self, task_id: str, payload: Mapping[str, object]) -> None: ...

class SlackAdapter(Protocol):
    def publish(self, task_id: str, event: Event, snapshot: TaskSnapshot, actions: Sequence[Action]) -> None: ...

class GitHubAdapter(Protocol):
    def observe_ci(self, task_id: str, payload: Mapping[str, object]) -> None: ...
```

- [ ] **Step 1: Add Protocol-only adapters**

No network clients, tokens, subprocess launchers, or Slack SDK dependency are added here.

- [ ] **Step 2: Document the concrete mapping**

In the design/README notes, state that `DISPATCH_EXECUTOR` should reuse the existing `[PASEO_TASK v1]` -> `.github/workflows/paseo-subagent.yml` -> `paseo-agent-runner` path, and Slack should publish coordinator events/actions into a single work-item thread.

- [ ] **Step 3: Run all unit tests again**

Expected: PASS with no new dependency installation.

---

### Task 5: Verify GREEN, document evidence, and keep the PR Draft

**Files:**
- Modify only task documentation if evidence needs to be recorded.

**Interfaces:**
- Consumes: the dedicated Actions run and branch tip.
- Produces: verifiable prototype checkpoint.

- [ ] **Step 1: Verify the dedicated workflow on the implementation tip**

Required job: `Paseo coordinator prototype / unit` is SUCCESS.

- [ ] **Step 2: Verify repository scope**

Changed paths must be limited to:

```text
.github/workflows/paseo-coordinator.yml
docs/superpowers/specs/2026-09-04-paseo-slack-coordinator-design.md
docs/superpowers/plans/2026-09-04-paseo-slack-coordinator-implementation.md
tools/paseo_coordinator/**
tests/paseo_coordinator/**
```

- [ ] **Step 3: Record RED -> GREEN evidence**

Add Issue #68 / Draft PR comment with the RED SHA/run and GREEN SHA/run. Do not claim live Slack-to-Paseo control is complete; this PR proves the deterministic coordination core and adapter boundary.

- [ ] **Step 4: Leave the PR Draft**

Do not merge or mark ready. The next separately approved work item is concrete integration of Slack events and the existing Paseo dispatch/cancel mechanisms against these adapter contracts.
