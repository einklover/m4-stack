---
name: m4-grok-astra
description: M4 stability workflow. Primary agent is Grok 4.7 zxl4869 on account grok-low. Astra High is a short read-only cross-check. Roles override docs/M4_ORCHESTRATION.md and docs/M4_AGENT_LESSONS.md. They do not override AGENTS.md hardware or data safety.
---

# M4 stability workflow

This skill is the role source for the M4 stability project. It overrides the older split in `docs/M4_ORCHESTRATION.md` and `docs/M4_AGENT_LESSONS.md` (Grok as merge/QEMU only, Muse as the default implementer). Hardware and data-safety rules in `AGENTS.md` stay binding.

## Milestones

ChatGPT web sets goals and accepts milestones only. It does not take a callback for every patch. Return to web at a milestone with a short stage note. Do not keep an unbounded session open for status.

## Primary agent

Grok 4.7, identity zxl4869, account grok-low, is the long-running primary. It audits, implements, and runs tests on its own inside the current worktree.

One round is one real issue: locate it, keep a source-verifiable evidence note, apply the minimal fix, run the targeted host test, then integrate and commit only what that round owns.

An optional second zxl4869 Grok may audit or implement a slice that does not share files with the primary. One writer per worktree. If a slice cannot be split by file, open another worktree. Never overwrite dirty edits.

## Astra

`codex/gpt-6-astra` with thinking high is a short, independent, read-only advisor. It does not implement, does not watch a terminal, and is not on the normal path.

Call Astra only for a hard cross-module deadlock, filesystem data loss, an unclear OOM, or the same defect after two repeated failed patches.

The handoff is one compact evidence packet for one issue. Every claim in it must be checkable in the tree:

- git SHA from `git rev-parse HEAD`
- `path:line` anchors that were read in the tree
- lock call graph (who takes which lock, and in what order)
- tests already run, with the command and the result
- the fix already on the tree, if any
- the single open question

Astra reads source and returns at most three findings plus the minimum acceptance check. Grok applies only that boundary and retests. If this session cannot create an Astra session, the web milestone owner dispatches it. Do not edit Paseo to force a dispatch.

## Muse

Muse is an optional fallback only when Grok is unavailable. Prefer the zxl4869 account. Do not switch to another account unless the user asks.

## Call relation and callback

Web owns the milestone. Grok owns the issue loop and calls back to web only when the milestone acceptance check is met or a stop condition fires. Astra is downstream of Grok and returns to Grok, not into a second implementation loop.

Escalate to web immediately when the same patch failed twice and Astra was not available, a test would touch a real SD card or flash a device, dirty files outside the slice would have to be overwritten, or the evidence packet cannot name a SHA and `path:line`. Do not poll agent lists. A finished or blocked worker notifies the Grok session that spawned it.

Stop conditions that end the round without another patch: missing source anchors, a test that was not actually run, or a result that would be reported as hardware proof.

## Loop

One issue, one minimal fix, one targeted test. Do not batch unrelated defects into the same edit.

At integration only, run the APP1 build and the QEMU build one after another, then an isolated SD smoke. Serialize those runs. The smoke shows that an isolated image boots. It is not hardware proof. Never claim a device result without a real-device command and artifact, and this workflow does not authorize that run.

A batch is closed only when the command output exists. A compile, host test, QEMU run, or device check that was not executed is not a pass.

## Acceptance

A milestone is acceptable only when the evidence packet names the git SHA, the `path:line` anchors, the lock call graph, and the tests that were run. Astra, when called, returns at most three findings and one minimum acceptance check, and Grok retests inside that boundary. Integration evidence is a serialized APP1 build, then a QEMU build, then an isolated SD smoke, and that smoke is not hardware proof.

## Safety

Do not overwrite dirty work. Do not touch a real SD card, flash a device, or edit Paseo. Do not `git reset` or `git clean`. Do not commit firmware binaries, `.pio`, credentials, or device captures. Do not kill `m4adb.py` with `pkill`. Leave older stage reports as they are.
