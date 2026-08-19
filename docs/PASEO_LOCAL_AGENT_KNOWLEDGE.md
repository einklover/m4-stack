# Paseo local-agent durable knowledge

Purpose: preserve reusable facts learned by the Mac mini / Paseo local agent so later tasks do not rediscover the same device, tooling, workflow, or debugging facts.

This is a **curated engineering runbook**, not a raw task log. GitHub Issues remain the source of truth for per-task status and acceptance evidence.

## Mandatory task behavior

Every dispatched local-agent task must do this before debugging:

1. Read `AGENTS.md`, `HANDOFF.md`, `docs/FAST_FIRMWARE_DEV.md`, and the active Issue.
2. Read this document from the task branch. If the task branch does not contain it yet, read the authoritative copy from `origin/main` with:

   ```bash
   git show origin/main:docs/PASEO_LOCAL_AGENT_KNOWLEDGE.md
   ```

3. Reuse verified paths, commands, device facts, and known solutions here instead of re-discovering them.
4. At task end, include a `knowledge_delta` section in `[PASEO_RESULT v1]`.
5. If the task discovered reusable knowledge, update this document in the task worktree and push that documentation change with the task result. Do not only mention it in chat/logs.
6. If nothing reusable was learned, report `knowledge_delta: none` and do not add noise to this file.

## What belongs here

Add concise entries for facts that will save work on a later task, such as:

- exact tool / binary / runner / cache / worktree locations;
- reliable build, flash, adb, m4adb, serial, network, or test commands;
- device identifiers and interfaces that are stable and non-secret;
- recurring failure signatures and their verified root causes;
- fixes/workarounds that were actually validated;
- ordering constraints, ownership rules, race conditions, or lifecycle traps;
- safety constraints that are easy to violate accidentally;
- how to collect high-value diagnostics without OCR or destructive operations.

Do **not** add:

- passwords, Wi-Fi credentials, API tokens, cookies, private keys, or other secrets;
- huge raw logs;
- speculative hypotheses that were not verified;
- one-off task status that belongs only in the Issue;
- copied prose that already exists in another authoritative document unless a short pointer is needed.

## Entry format

Append under the appropriate section using this compact shape:

```text
### YYYY-MM-DD — <topic> — <task_id>
Context: <repo/branch/head and device if relevant>
Observed: <failure/signature/fact>
Root cause: <verified cause, or "unknown" if still unresolved>
Reuse: <exact command/path/procedure to reuse next time>
Caution: <important boundary/safety/race, if any>
Evidence: <Issue number / commit / test name / counter; no raw secrets>
```

When a newer entry supersedes an older one, edit the old entry to point at the new rule instead of leaving two conflicting instructions.

---

## Persistent environment / tooling

### 2026-08-19 — Paseo runner layout — dispatcher baseline
Context: `einklover/m4-stack`, self-hosted Mac mini runner.
Observed: task execution is isolated from the interactive checkout.
Root cause: n/a.
Reuse: runner service `~/actions-runner-m4`; installed wrapper `~/.local/bin/paseo-agent-runner`; durable runner state `~/.paseo-agent` -> `/Volumes/z/paseo-agent`; independent runner clone `/Volumes/z/paseo-agent/repos/einklover-m4-stack`; per-task worktrees under `/Volumes/z/paseo/m4crosspoint/agent-worktrees/<task_id>`; persistent per-Issue agent state under `/Volumes/z/paseo/m4crosspoint/agent-state/`.
Caution: never modify the user's interactive `work/m4-stack` checkout from a dispatched task; no force push.
Evidence: dispatcher contract / prior Browser Bridge tasks.

### 2026-08-19 — GitHub dispatcher execution model — dispatcher baseline
Context: `.github/workflows/paseo-subagent.yml` on repository default branch.
Observed: `issue_comment` dispatch runs only from the workflow present on `main`; task comments are prompts, not shell snippets.
Root cause: GitHub Actions `issue_comment` semantics and runner hardening.
Reuse: comments start with `[PASEO_TASK v1]`; use a unique `task_id`; verify semantic nested `[PASEO_RESULT v1]` rather than treating outer process completion as success.
Caution: outer wrapper/process PASS can coexist with semantic `BLOCKED`/`PARTIAL`; always inspect the structured result body.
Evidence: `docs/PASEO_SUBAGENT_DISPATCHER.md`.

## Murphy M4 device / flash

### 2026-08-19 — production flashing safety — Browser Bridge validated baseline
Context: Murphy M4 ESP32-S3 production firmware.
Observed: Browser Bridge device work uses the production `murphy_m4` image and APP1.
Root cause: partition layout / safety policy.
Reuse: use repository APP1 flash tooling; application offset is `0x6e0000`; hash-verify the produced image and select OTA slot 1 after flashing.
Caution: never flash APP0, bootloader, partition table, QEMU profiles, or perform a full erase/factory reset unless the user explicitly authorizes it.
Evidence: `AGENTS.md`, Browser Bridge #30/#33/#34/#39 validation.

### 2026-08-19 — m4adb ownership — stable infrastructure
Context: physical M4 automation/debugging.
Observed: multiple m4adb/serial daemons can collide and produce misleading device behavior.
Root cause: competing ownership of the same device/control channel.
Reuse: use repository m4adb tooling and keep one global daemon.
Caution: never `pkill -f m4adb.py`; stop/restart through the repository-supported mechanism and record which process owns the device before debugging transport/display failures.
Evidence: `AGENTS.md` and prior Browser Bridge white-panel diagnostics.

## Browser Bridge

### 2026-08-19 — physical panel baseline is not framebuffer acceptance — #33/#34
Context: M4B3 Browser Bridge panel presenter.
Observed: Android can send a non-white frame and M4 can accept the logical CRC while the glass remains stale/white.
Root cause: physical panel baseline/lifecycle can become untrusted independently from the accepted logical framebuffer.
Reuse: when the panel may have been changed/reset outside Browser Bridge, invalidate the last physical Browser baseline; next Browser presentation must use the production recovery FULL path, and only a successful physical present may restore trust.
Caution: `FRAME_ACK` means logical framebuffer/protocol acceptance only; never couple it to physical E-ink completion. Do not infer optical success from CRC/counters alone.
Evidence: #33 white-glass diagnosis and #34 refresh-hygiene validation.

### 2026-08-19 — Browser Bridge touch ownership — #33
Context: M4 physical touch -> Android hidden WebView.
Observed: FT6x36 touch must be routed exclusively to Browser Bridge while the M4B3 session is hello-ok, otherwise local Reader/Home gestures can also consume it.
Root cause: shared physical input source.
Reuse: `MappedInputManager::setTouchRoutedToBrowser(true)` suppresses the ordinary local touch path while `M4B3Tcp::captureFromGpio()` sends the pointer over M4B3. Panel-native to logical mapping is `logicalX = 479 - physicalY`, `logicalY = physicalX`.
Caution: Android dispatch stays inside the app-owned hidden WebView; no Accessibility/root/adb/global injection as product behavior.
Evidence: #33 M4 touch-return implementation.

### 2026-08-19 — discovery precedence — #39
Context: Android Browser Bridge LAN auto-connect.
Observed: validated selection precedence is manual override > live discovered `_m4b3._tcp` > cached last-known endpoint > none; explicit `loopback`/`local` bypasses discovery.
Root cause: deterministic control-plane design.
Reuse: keep `_m4b3._tcp` mDNS/DNS-SD as control-plane only; M4B3 framing, ACK, panel and input semantics are independent. Empty `m4b3_host` is normal AUTO mode.
Caution: avoid listener/retry storms; discovery restarts are bounded. Manual host remains the deterministic escape hatch.
Evidence: #39 device AUTO/manual/service-restart/M4-reboot PASS.
