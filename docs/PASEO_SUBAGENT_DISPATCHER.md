# Paseo Remote Subagent Dispatcher v1

Remote main agents send work to this Mac mini through GitHub Issue comments.
The Mac mini only makes **outbound** connections to GitHub. No public port,
Cloudflare Tunnel, or VPS control plane is used in v1.

```text
Remote main agent
    -> GitHub Issue comment  [PASEO_TASK v1]
    -> GitHub Actions (issue_comment)
    -> self-hosted macOS ARM64 runner (paseo, m4-lab)
    -> paseo-agent-runner
    -> Paseo CLI (`paseo run --provider grok`)
    -> Grok Build (`grok agent --reasoning-effort high stdio`)
    -> git / adb / build
    -> GitHub Issue comment  [PASEO_RESULT v1]
```

## Trigger

The comment body must **start with** exactly:

```text
[PASEO_TASK v1]
```

Only allowlisted GitHub users can dispatch. Default allowlist:

```text
einklover
```

Override with repository Actions variable `PASEO_DISPATCH_ALLOWLIST`
(comma-separated logins).

The workflow file must exist on the repository **default branch** (`main`).
The job does not download `actions/checkout`; it calls the Mac mini
install of `paseo-agent-runner` so GitHub 429s cannot block dispatch.
`issue_comment` workflows do not run from feature branches.

## Task shape

```text
[PASEO_TASK v1]

task_id: dispatcher-smoke-001
issue: 27
repo: einklover/m4-stack
branch: agent/eink-browser-bridge
mode: inspect-only

goal:
...
```

`task_id` is required and unique. Repeating it posts
`duplicate task ignored` and does not start a second agent.

`mode: inspect-only` forbids commit/push from the orchestrator.

Optional: `expected_head`, `timeout` (`60m` default, `180m` cap),
`provider` (`grok` default).

The comment is a prompt, never a shell script. The workflow writes it to a
file through an environment variable and never interpolates it into bash.

## Durable knowledge handoff

Every local-agent task must reuse and, when needed, extend the repository runbook:

```text
docs/PASEO_LOCAL_AGENT_KNOWLEDGE.md
```

Before debugging, the agent reads that file from the task branch. If the branch predates the file, it reads the authoritative copy from `origin/main`:

```bash
git show origin/main:docs/PASEO_LOCAL_AGENT_KNOWLEDGE.md
```

Every `[PASEO_RESULT v1]` must contain a `knowledge_delta` field/section:

- `knowledge_delta: none` when the run learned nothing reusable;
- otherwise summarize the new reusable fact(s) and update `docs/PASEO_LOCAL_AGENT_KNOWLEDGE.md` in the task worktree.

Reusable knowledge includes verified tool paths, exact commands, recurring failure signatures, root causes, validated fixes, device/lifecycle traps, and safety/ordering constraints. Do not store secrets, credentials, huge raw logs, or unverified guesses.

When the run produces reusable knowledge, the documentation update is part of the task deliverable and must be committed/pushed with the branch before the final result is posted. The final result must name the knowledge-doc commit/path so the remote main agent can review it. Per-task status and acceptance evidence still belong in the GitHub Issue; the knowledge file is only for facts worth reusing later.

## Local layout

| Path | Role |
|------|------|
| `~/actions-runner-m4` | GitHub Actions runner on the boot disk. launchd cannot execute from `/Volumes/z`. |
| `~/.paseo-agent` | Task/lock/log/worktree state (symlink to `/Volumes/z/paseo-agent`) |
| `~/.local/bin/paseo-agent-runner` | CLI wrapper |
| `scripts/paseo_agent/paseo_agent_runner.py` | Versioned orchestrator |

## CLI

```bash
paseo-agent-runner status
paseo-agent-runner --task /path/to/task.txt --issue 27
paseo-agent-runner cancel <task_id>
```

## Runner service

Install directory: `~/actions-runner-m4`  
Name: `mac-mini-m4`  
Labels: `self-hosted`, `macOS`, `ARM64`, `paseo`, `m4-lab`

```bash
cd ~/actions-runner-m4
./svc.sh status
./svc.sh stop
./svc.sh start
```

User launchd label (created by `svc.sh install`):

```text
actions.runner.einklover-m4-stack.mac-mini-m4
```

Logs: `~/actions-runner-m4/_diag/`

## Paseo invocation

```text
paseo run --provider grok --cwd <task-worktree> --new-workspace local --wait-timeout <N>s --json
```

This starts a **new** local workspace so it does not steal an interactive
Paseo tab. It does not `pkill` other Paseo/Grok sessions.

## Security

- Allowlisted comment authors only
- Comment body is never `eval`'d or passed to a shell
- One repository lock; stale PID locks are discarded
- v1 accepts only `einklover/m4-stack`
- Provider allowlist is the configured Grok/Paseo backends
- Agent prompt forbids workflow self-modification, force-push, wipe/factory reset
- Job uses `permissions: contents: write, issues: write` only

## Operations notes

Do not flash QEMU firmware profiles to hardware.
Do not expand m4sim unless a production firmware change proves a simulator gap.
Keep large HTTP/font work streaming and PSRAM-aware.