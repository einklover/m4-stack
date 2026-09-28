---
name: m4-development-workflow
description: Canonical low-cost M4 development, independent pre-review and opt-in architecture audit.
---

# M4 development workflow

Authority: \`AGENTS.md\` hardware/data safety always wins. This is the only
current M4 role/stage contract and supersedes historical orchestration text.
ChatGPT web coordinates milestone acceptance. Do not create extra infrastructure.

## 1. Smallest complete V1 — Grok zxl4869 Low

Start from the user request and a measurable acceptance condition. Record the
base SHA, exact file ownership and risk (low/high), and preserve all existing
dirty work. Use \`grok-low/grok-4.7\`, **zxl4869**, in one isolated worktree;
one writer per worktree. Another writer is allowed only for disjoint files.
Do not change account, touch Paseo source, or flash hardware without permission.

Implement a coarse but complete runnable first version. Before it works, do
not add a framework, speculative optimization, mass refactor, architecture
review, new agents or elaborate test infrastructure. Run the smallest relevant
host/contract test. If red, make one minimal correction and rerun. Actual
command, exit status and expected behavior are evidence; invented PASS is not.

## 2. Independent Muse High pre-review

Once V1 and its targeted test are green, freeze the SHA and provide only its
diff, affected production callers, identified failure paths and test evidence
to Muse High **read-only** in a separate worktree. Muse reports concrete
path:line, reachable trigger, and the smallest acceptance test. It does not
edit, dispatch or manage Grok. Hand confirmed blockers back to Grok, rerun
the affected tests and review only changed boundaries. Do not claim a Muse
review without a real cited result. The integrator may explicitly waive it
for a strictly documentation-only change.

## 3. Integration gate

Integrate only the reviewed tested SHA. Record the base/frozen/final SHA,
changed files, real targeted host commands/results and review provenance.
Run \`git diff --check\`; use relevant dependency checks when needed.
For firmware/runtime changes, compile sequentially:
\`pio run -e murphy_m4 -j1\` then \`pio run -e murphy_m4_qemu -j1\`.
Plugin-only changes should run their packaging/manifest tests, not full PIO
unless a firmware/runtime interface changed. CI checks PR evidence format and
runs necessary actual tests/builds. Only the human integrator confirms Muse
provenance and authorizes any merge. Never merge a huge historical feature
branch wholesale into \`main\` in place of a scoped diff.

## 4. Astra gate — OFF unless the user explicitly opts in

No worker, workflow, schedule, reminder or coordinator may summon Astra,
check its quota, retry after quota exhaustion or automatically resume its
audit. Explicit user authorization must name the frozen SHA and one narrow
cross-module risk (deadlock, OOM, data loss, recovery), and remains limited
to that audit. If authorized, Astra High is an independent **read-only**
reviewer, not an implementer or subagent orchestrator.

Give it one compact evidence packet: SHA, changed path:line anchors, relevant
lock/ownership call graph if applicable, observed failure and targeted test
outcome, Muse report, and one open question. Ask for at most three proven
P0/P1 findings with reachable preconditions and minimal tests. Batch any
fixes under Grok then Muse; permit **at most one differential** follow-up
of affected callers if needed. Quota exhausted? Stop; no automatic retry.

## 5. Evidence, stop and safety

Use the PR template. Distinguish host test, successful compilation, QEMU boot
and physical device observation. An unexecuted test is unverified, not PASS.
High-risk changes keep fault-injection cases and last recovery copies safe.
No automatic flashing, secret capture, Paseo edits or \`main\` merge.
At the first green acceptance gate, **stop**. Put optional improvements in
the backlog rather than reopening reviews on unchanged code.
