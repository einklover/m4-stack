#!/usr/bin/env python3
"""Paseo Remote Subagent Dispatcher v1 orchestrator.

GitHub Issue comments are treated as AI prompts only. This process never
eval()/exec()s comment text and never interpolates it into a shell.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import json
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Mapping

TASK_MARKER = "[PASEO_TASK v1]"
RESULT_MARKER = "[PASEO_RESULT v1]"
DEFAULT_REPO = "einklover/m4-stack"
DEFAULT_PROVIDER = "grok"
DEFAULT_TIMEOUT_SEC = 60 * 60
MAX_TIMEOUT_SEC = 180 * 60
ALLOWED_PROVIDERS = {
    "grok",
    "grok-medium",
    "grok-low",
    "grok-second",
    "grok-second-medium",
    "grok-second-low",
}
ALLOWED_REPOS = {DEFAULT_REPO}
TASK_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]{1,200}$")
KNOWN_KEYS = (
    "task_id",
    "issue",
    "repo",
    "branch",
    "mode",
    "expected_head",
    "timeout",
    "provider",
    "goal",
    "context",
    "acceptance",
    "constraints",
    "expected_output",
    "do",
)
INSPECT_ONLY_MODES = {"inspect-only", "inspect", "read-only"}
RESULT_FILE_NAME = ".paseo-agent-result.md"
RUNNER_NAME = "mac-mini-m4"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def home_state_dir() -> Path:
    override = os.environ.get("PASEO_AGENT_HOME")
    if override:
        return Path(override).expanduser()
    return Path.home() / ".paseo-agent"


def which(name: str) -> str | None:
    from shutil import which as _which

    return _which(name)


def run_cmd(
    argv: list[str],
    *,
    cwd: Path | None = None,
    timeout: int | None = None,
    check: bool = False,
    env: Mapping[str, str] | None = None,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    result = subprocess.run(
        argv,
        cwd=str(cwd) if cwd else None,
        timeout=timeout,
        text=True,
        input=input_text,
        capture_output=True,
        env=merged,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {shlex.join(argv)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


class Logger:
    def __init__(self, path: Path, task_id: str = "-", extra: Mapping[str, str] | None = None):
        self.path = path
        self.task_id = task_id
        self.extra = dict(extra or {})
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def log(self, stage: str, message: str, **fields: Any) -> None:
        payload = {
            "timestamp": utc_now(),
            "task_id": self.task_id,
            "pid": os.getpid(),
            "stage": stage,
            "message": message,
        }
        payload.update(self.extra)
        payload.update(fields)
        line = " ".join(f"{k}={_fmt(v)}" for k, v in payload.items())
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
        print(line, flush=True)


def _fmt(value: Any) -> str:
    text = "" if value is None else str(value)
    if text == "" or any(ch.isspace() for ch in text):
        return json.dumps(text, ensure_ascii=False)
    return text


@dataclass
class ParsedTask:
    raw: str
    task_id: str
    issue: int | None = None
    repo: str = DEFAULT_REPO
    branch: str = "main"
    mode: str = "implement-and-test"
    expected_head: str | None = None
    timeout_sec: int = DEFAULT_TIMEOUT_SEC
    provider: str = DEFAULT_PROVIDER
    fields: dict[str, str] = field(default_factory=dict)

    def is_inspect_only(self) -> bool:
        return self.mode.strip().lower() in INSPECT_ONLY_MODES


class ParseError(ValueError):
    pass


def parse_timeout(value: str) -> int:
    text = value.strip().lower()
    if not text:
        return DEFAULT_TIMEOUT_SEC
    match = re.fullmatch(r"(\d+)\s*([smh])?", text)
    if not match:
        raise ParseError(f"invalid timeout: {value!r}")
    amount = int(match.group(1))
    unit = match.group(2) or "s"
    seconds = amount * {"s": 1, "m": 60, "h": 3600}[unit]
    if seconds < 30:
        raise ParseError("timeout must be at least 30s")
    return min(seconds, MAX_TIMEOUT_SEC)


def parse_task(body: str) -> ParsedTask:
    text = body.replace("\r\n", "\n")
    idx = text.find(TASK_MARKER)
    if idx < 0:
        raise ParseError(f"missing {TASK_MARKER}")
    remainder = text[idx + len(TASK_MARKER) :]
    if remainder.startswith("\n"):
        remainder = remainder[1:]

    values: dict[str, list[str]] = {}
    current: str | None = None
    for raw_line in remainder.split("\n"):
        line = raw_line.rstrip()
        key_match = re.match(r"^([A-Za-z][A-Za-z0-9_]*)\s*:\s*(.*)$", line)
        if key_match:
            key = key_match.group(1).lower()
            if key in KNOWN_KEYS:
                current = key
                values.setdefault(current, [])
                rest = key_match.group(2)
                if rest != "":
                    values[current].append(rest)
                continue
        if current is None:
            continue
        values.setdefault(current, []).append(line)

    def joined(key: str) -> str:
        return "\n".join(values.get(key, [])).strip()

    task_id = joined("task_id")
    if not TASK_ID_RE.fullmatch(task_id):
        raise ParseError("task_id is required and must match [A-Za-z0-9._-]{1,80}")

    repo = joined("repo") or DEFAULT_REPO
    if repo not in ALLOWED_REPOS:
        raise ParseError(f"repo not allowed in v1: {repo}")

    branch = joined("branch") or "main"
    if not BRANCH_RE.fullmatch(branch) or ".." in branch or branch.startswith("-"):
        raise ParseError(f"invalid branch: {branch}")

    issue_text = joined("issue")
    issue = None
    if issue_text:
        issue_match = re.search(r"(\d+)", issue_text)
        if not issue_match:
            raise ParseError(f"invalid issue: {issue_text}")
        issue = int(issue_match.group(1))

    provider = joined("provider") or DEFAULT_PROVIDER
    if provider not in ALLOWED_PROVIDERS:
        raise ParseError(f"provider not allowed: {provider}")

    timeout_text = joined("timeout")
    timeout_sec = parse_timeout(timeout_text) if timeout_text else DEFAULT_TIMEOUT_SEC
    expected_head = joined("expected_head") or None
    mode = joined("mode") or "implement-and-test"
    fields = {key: joined(key) for key in values}

    return ParsedTask(
        raw=text,
        task_id=task_id,
        issue=issue,
        repo=repo,
        branch=branch,
        mode=mode,
        expected_head=expected_head,
        timeout_sec=timeout_sec,
        provider=provider,
        fields=fields,
    )


class RepoLock:
    def __init__(self, path: Path, task_id: str):
        self.path = path
        self.task_id = task_id
        self._fh: Any = None

    def acquire(self) -> dict[str, Any] | None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = self.path.open("a+", encoding="utf-8")
        try:
            fcntl.flock(self._fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self._fh.seek(0)
            try:
                existing = json.loads(self._fh.read() or "{}")
            except json.JSONDecodeError:
                existing = {"task_id": "unknown"}
            self._fh.close()
            self._fh = None
            if existing.get("pid") and not _pid_alive(int(existing["pid"])):
                return self._steal_stale(existing)
            return existing
        self._write()
        return None

    def _steal_stale(self, existing: Mapping[str, Any]) -> dict[str, Any] | None:
        # Previous holder died. Re-open and take the lock.
        self._fh = self.path.open("a+", encoding="utf-8")
        fcntl.flock(self._fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        self._write()
        existing = dict(existing)
        existing["_stale"] = True
        return None

    def update(self, **fields: Any) -> None:
        if self._fh is None:
            return
        self._fh.seek(0)
        try:
            data = json.loads(self._fh.read() or "{}")
        except json.JSONDecodeError:
            data = {}
        data.update(fields)
        self._fh.seek(0)
        self._fh.truncate()
        json.dump(data, self._fh, indent=2)
        self._fh.write("\n")
        self._fh.flush()

    def _write(self) -> None:
        payload = {
            "pid": os.getpid(),
            "pgid": os.getpgid(0),
            "task_id": self.task_id,
            "started_at": utc_now(),
            "host": socket.gethostname(),
        }
        self._fh.seek(0)
        self._fh.truncate()
        json.dump(payload, self._fh, indent=2)
        self._fh.write("\n")
        self._fh.flush()

    def release(self) -> None:
        if self._fh is None:
            return
        try:
            fcntl.flock(self._fh.fileno(), fcntl.LOCK_UN)
        finally:
            self._fh.close()
            self._fh = None
            try:
                self.path.unlink()
            except FileNotFoundError:
                pass


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


class GitHubIssueResultSink:
    def __init__(self, repo: str, issue: int | None):
        self.repo = repo
        self.issue = issue

    def post(self, body: str) -> None:
        if not self.issue:
            print("no issue number; skipping GitHub writeback", file=sys.stderr)
            return
        payload = {"body": body}
        result = run_cmd(
            [
                "gh",
                "api",
                "--method",
                "POST",
                f"repos/{self.repo}/issues/{self.issue}/comments",
                "--input",
                "-",
            ],
            input_text=json.dumps(payload),
        )
        if result.returncode != 0:
            raise RuntimeError(f"failed to post issue comment: {result.stderr or result.stdout}")


def format_result(fields: Mapping[str, Any]) -> str:
    lines = [RESULT_MARKER, ""]
    for key, value in fields.items():
        if value is None or value == "":
            continue
        if isinstance(value, list):
            lines.append(f"{key}:")
            for item in value:
                lines.append(f"- {item}")
            lines.append("")
        elif isinstance(value, str) and "\n" in value:
            lines.append(f"{key}:")
            lines.append(value.rstrip())
            lines.append("")
        else:
            lines.append(f"{key}: {value}")
    return "\n".join(lines).rstrip() + "\n"


def load_task_state(state_dir: Path, task_id: str) -> dict[str, Any] | None:
    path = state_dir / "tasks" / f"{task_id}.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def save_task_state(state_dir: Path, task_id: str, payload: Mapping[str, Any]) -> None:
    path = state_dir / "tasks" / f"{task_id}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    data = dict(payload)
    data["updated_at"] = utc_now()
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def collect_adb() -> dict[str, Any]:
    adb = which("adb")
    if not adb:
        return {"available": False, "summary": "adb not found", "raw": ""}
    run_cmd([adb, "start-server"], timeout=20)
    devices = run_cmd([adb, "devices", "-l"], timeout=20)
    raw = (devices.stdout or "") + (devices.stderr or "")
    rows = []
    for line in raw.splitlines():
        if not line.strip() or line.startswith("List of devices"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        serial, state = parts[0], parts[1]
        rows.append({"serial": serial, "state": state, "line": line.strip()})
    online = [r for r in rows if r["state"] == "device"]
    expected = [r for r in online if r["serial"] == "ZY22KN7WSK"]
    if expected:
        summary = "Android test device is available. serial=ZY22KN7WSK model=XT2437_4"
    elif online:
        summary = "Android device(s) available: " + ", ".join(
            f"{r['serial']} ({r['state']})" for r in online
        )
    elif rows:
        summary = "Android device present but not ready: " + ", ".join(
            f"{r['serial']}={r['state']}" for r in rows
        )
    else:
        summary = "No Android devices attached."
    return {"available": bool(online), "summary": summary, "raw": raw.strip(), "rows": rows}


def prepare_worktree(
    state_dir: Path, task: ParsedTask, log: Logger
) -> tuple[Path, str]:
    repos = state_dir / "repos"
    repo_dir = repos / task.repo.replace("/", "-")
    worktree = state_dir / "worktrees" / task.task_id
    repos.mkdir(parents=True, exist_ok=True)
    if not (repo_dir / ".git").exists():
        log.log("PREPARE", "cloning repository", repo=task.repo)
        run_cmd(
            ["git", "clone", "--", f"https://github.com/{task.repo}.git", str(repo_dir)],
            check=True,
            timeout=300,
        )
    log.log("PREPARE", "fetching origin", repo=str(repo_dir))
    run_cmd(["git", "fetch", "origin", "--prune"], cwd=repo_dir, check=True, timeout=180)
    remote_ref = f"refs/remotes/origin/{task.branch}"
    head = run_cmd(
        ["git", "rev-parse", remote_ref],
        cwd=repo_dir,
        timeout=30,
    )
    if head.returncode != 0:
        raise RuntimeError(f"remote branch not found: origin/{task.branch}")
    head_sha = head.stdout.strip()
    if task.expected_head and not (
        head_sha.startswith(task.expected_head) or task.expected_head.startswith(head_sha)
    ):
        raise HeadMismatch(head_sha)
    if worktree.exists():
        existing = run_cmd(["git", "rev-parse", "--is-inside-work-tree"], cwd=worktree, timeout=20)
        if existing.returncode == 0:
            run_cmd(["git", "fetch", "origin", "--prune"], cwd=worktree, timeout=180)
            run_cmd(["git", "checkout", "--detach", head_sha], cwd=worktree, check=True, timeout=60)
            return worktree, head_sha
        raise RuntimeError(f"worktree path exists but is not a git worktree: {worktree}")
    run_cmd(
        ["git", "worktree", "add", "--detach", str(worktree), head_sha],
        cwd=repo_dir,
        check=True,
        timeout=120,
    )
    return worktree, head_sha


class HeadMismatch(RuntimeError):
    def __init__(self, actual: str):
        super().__init__(actual)
        self.actual = actual


def build_prompt(task: ParsedTask, worktree: Path, adb: Mapping[str, Any], meta: Mapping[str, Any]) -> str:
    issue = task.issue if task.issue is not None else "unspecified"
    inspect_rule = (
        "This task is inspect-only. Do not modify files, do not commit, do not push."
        if task.is_inspect_only()
        else "Commit only intentional in-scope changes with a specific message. Do not force-push."
    )
    return f"""You are the local execution subagent for repository {task.repo}.

You have access to:
- this dedicated git worktree
- git
- GitHub CLI
- Android SDK / adb
- connected Android device when available
- project build tools

Worktree:
{worktree}

Rules:
- first read AGENTS.md, HANDOFF.md and the relevant GitHub Issue
- inspect existing code before modifying
- do not reimplement completed work
- do not modify unrelated files
- preserve branch boundaries; stay on the requested branch
- run relevant tests when the task requires them
- if an Android device is available, perform real-device validation only when requested
- never claim a device test passed unless command output proves it
- {inspect_rule}
- push only to the requested branch, never force-push
- do not modify .github/workflows/ unless the task is explicitly about the dispatcher
- do not factory reset, wipe data, change bootloader, or run destructive adb rm -rf
- do not pkill Paseo or other interactive sessions
- do not sudo-install unknown system components
- do not modify SSH, Cloudflare, or host network configuration
- summarize exact evidence at the end
- when finished, write a concise markdown file named {RESULT_FILE_NAME} in the worktree with:
  status, summary, files_changed, tests, device, device_evidence, acceptance, remaining, recommended_next
- do not dump full logs into that file

Execution context:
- task_id: {task.task_id}
- issue: {issue}
- repo: {task.repo}
- requested_branch: {task.branch}
- mode: {task.mode}
- head_before: {meta.get("head_before")}
- github_run: {meta.get("run_url") or "local"}
- comment_id: {meta.get("comment_id") or "n/a"}
- adb: {adb.get("summary")}
- adb_raw:
{adb.get("raw") or "(none)"}

The following task was issued by the remote main agent:

<TASK>
{task.raw.strip()}
</TASK>
"""


def git_snapshot(worktree: Path) -> dict[str, str]:
    def _git(*args: str) -> str:
        result = run_cmd(["git", *args], cwd=worktree, timeout=60)
        return (result.stdout or "").strip()

    return {
        "head": _git("rev-parse", "HEAD"),
        "status": _git("status", "--short", "--branch"),
        "diff_stat": _git("diff", "--stat"),
        "unpushed": _git("log", "--oneline", f"origin/{_current_branch(worktree)}..HEAD")
        if _current_branch(worktree)
        else _git("log", "--oneline", "-5"),
    }


def _current_branch(worktree: Path) -> str | None:
    result = run_cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=worktree, timeout=20)
    name = (result.stdout or "").strip()
    if not name or name == "HEAD":
        return None
    return name


def maybe_push(task: ParsedTask, worktree: Path, log: Logger) -> tuple[bool, str]:
    if task.is_inspect_only():
        return False, "inspect-only; push skipped"
    ahead = run_cmd(
        ["git", "rev-list", "--count", f"origin/{task.branch}..HEAD"],
        cwd=worktree,
        timeout=30,
    )
    count_text = (ahead.stdout or "0").strip()
    try:
        count = int(count_text)
    except ValueError:
        count = 0
    if ahead.returncode != 0 or count <= 0:
        return False, "no unpushed commits"
    log.log("PUSH", "pushing commits", count=count, branch=task.branch)
    pushed = run_cmd(
        ["git", "push", "origin", f"HEAD:refs/heads/{task.branch}"],
        cwd=worktree,
        timeout=180,
    )
    if pushed.returncode != 0:
        raise RuntimeError(f"git push failed: {pushed.stderr or pushed.stdout}")
    return True, f"pushed {count} commit(s) to origin/{task.branch}"


def extract_agent_id(text: str) -> str | None:
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        for key in ("Id", "id", "agentId", "agent_id"):
            value = data.get(key)
            if value:
                return str(value)
    match = re.search(
        r"\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b",
        text,
        re.I,
    )
    return match.group(0) if match else None


def run_paseo(
    task: ParsedTask,
    worktree: Path,
    prompt: str,
    log: Logger,
    lock: RepoLock,
    log_path: Path,
) -> dict[str, Any]:
    paseo = which("paseo")
    if not paseo:
        raise RuntimeError("paseo CLI not found on PATH")
    title = f"paseo {task.task_id}"[:60]
    argv = [
        paseo,
        "run",
        "--provider",
        task.provider,
        "--title",
        title,
        "--cwd",
        str(worktree),
        "--new-workspace",
        "local",
        "--wait-timeout",
        f"{task.timeout_sec}s",
        "--label",
        f"task_id={task.task_id}",
        "--label",
        "source=paseo-agent-runner",
        "--json",
        prompt,
    ]
    log.log("PASEO", "starting paseo run", provider=task.provider, cwd=str(worktree))
    with log_path.open("a", encoding="utf-8") as fh:
        fh.write(f"\n# PASEO argv (prompt omitted): {shlex.join(argv[:-1])} <PROMPT>\n")
        proc = subprocess.Popen(
            argv,
            cwd=str(worktree),
            stdout=fh,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            text=True,
        )
        lock.update(paseo_pid=proc.pid, paseo_pgid=os.getpgid(proc.pid))
        try:
            code = proc.wait(timeout=task.timeout_sec + 30)
        except subprocess.TimeoutExpired:
            _stop_pgid(os.getpgid(proc.pid))
            raise TimeoutError("paseo run exceeded timeout")
    output = log_path.read_text(encoding="utf-8", errors="replace")
    agent_id = extract_agent_id(output[-8000:])
    if agent_id:
        lock.update(agent_id=agent_id)
    inspect = {}
    if agent_id:
        inspected = run_cmd([paseo, "inspect", "--json", agent_id], timeout=30)
        try:
            inspect = json.loads(inspected.stdout or "{}")
        except json.JSONDecodeError:
            inspect = {"raw": inspected.stdout}
    return {
        "exit_code": code,
        "agent_id": agent_id,
        "inspect": inspect,
    }


def _stop_pgid(pgid: int) -> None:
    if pgid <= 0:
        return
    try:
        os.killpg(pgid, signal.SIGTERM)
    except OSError:
        return
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            os.killpg(pgid, 0)
        except OSError:
            return
        time.sleep(0.2)
    try:
        os.killpg(pgid, signal.SIGKILL)
    except OSError:
        return


def collect_agent_logs(agent_id: str | None) -> str:
    if not agent_id:
        return ""
    paseo = which("paseo")
    if not paseo:
        return ""
    result = run_cmd([paseo, "logs", "--tail", "40", "--filter", "text", agent_id], timeout=30)
    text = (result.stdout or "").strip()
    if len(text) > 2500:
        text = text[-2500:]
    return text


def read_result_file(worktree: Path) -> str:
    path = worktree / RESULT_FILE_NAME
    if not path.exists():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    if len(text) > 4000:
        return text[:4000] + "\n...[truncated]..."
    return text


def dispatch(args: argparse.Namespace) -> int:
    state_dir = home_state_dir()
    for name in ("tasks", "results", "logs", "locks", "worktrees", "repos"):
        (state_dir / name).mkdir(parents=True, exist_ok=True)

    body = Path(args.task).read_text(encoding="utf-8")
    sink_issue = args.issue
    try:
        task = parse_task(body)
    except ParseError as exc:
        issue_no = sink_issue or 0
        sink = GitHubIssueResultSink(DEFAULT_REPO, issue_no or None)
        sink.post(
            format_result(
                {
                    "task_id": "unparsed",
                    "status": "FAIL",
                    "failure_stage": "RECEIVE",
                    "error_summary": str(exc),
                    "runner": RUNNER_NAME,
                }
            )
        )
        return 2

    if args.issue:
        task.issue = args.issue

    log = Logger(
        state_dir / "logs" / f"{task.task_id}.log",
        task_id=task.task_id,
        extra={"issue": task.issue or "", "repo": task.repo, "branch": task.branch},
    )
    log.log("RECEIVE", "accepted task", source=args.source, comment_id=args.comment_id)
    sink = GitHubIssueResultSink(task.repo, task.issue)
    existing = load_task_state(state_dir, task.task_id)
    if existing and existing.get("status") in {"PASS", "FAIL", "BLOCKED", "RUNNING"}:
        running_pid = int(existing.get("pid") or 0)
        if existing.get("status") == "RUNNING" and _pid_alive(running_pid):
            sink.post(
                format_result(
                    {
                        "task_id": task.task_id,
                        "status": "BUSY",
                        "reason": "duplicate task already running",
                        "pid": running_pid,
                    }
                )
            )
            return 0
        if existing.get("status") in {"PASS", "FAIL", "BLOCKED"}:
            sink.post(
                format_result(
                    {
                        "task_id": task.task_id,
                        "status": existing.get("status"),
                        "reason": "duplicate task ignored",
                        "previous_status": existing.get("status"),
                        "local_log": str(state_dir / "logs" / f"{task.task_id}.log"),
                    }
                )
            )
            return 0

    lock = RepoLock(state_dir / "locks" / f"{task.repo.replace('/', '-')}.lock", task.task_id)
    log.log("LOCK", "acquiring repository lock")
    busy = lock.acquire()
    if busy:
        sink.post(
            format_result(
                {
                    "task_id": task.task_id,
                    "status": "BUSY",
                    "reason": "another task is running",
                    "active_task": busy.get("task_id"),
                    "active_pid": busy.get("pid"),
                }
            )
        )
        return 0

    started = utc_now()
    save_task_state(
        state_dir,
        task.task_id,
        {
            "status": "RUNNING",
            "pid": os.getpid(),
            "issue": task.issue,
            "repo": task.repo,
            "branch": task.branch,
            "started_at": started,
            "comment_id": args.comment_id,
        },
    )
    sink.post(
        format_result(
            {
                "task_id": task.task_id,
                "status": "RUNNING",
                "runner": RUNNER_NAME,
                "started_at": started,
                "branch": task.branch,
                "repo": task.repo,
                "comment_id": args.comment_id,
                "run_url": args.run_url,
            }
        )
    )

    status = "FAIL"
    failure_stage = ""
    error_summary = ""
    pushed = False
    head_before = ""
    worktree: Path | None = None
    agent_id = None
    try:
        adb = collect_adb()
        log.log("PREPARE", adb["summary"])
        worktree, head_before = prepare_worktree(state_dir, task, log)
        if task.expected_head and not (
            head_before.startswith(task.expected_head) or task.expected_head.startswith(head_before)
        ):
            raise HeadMismatch(head_before)
        prompt_path = state_dir / "tasks" / f"{task.task_id}.prompt.txt"
        prompt = build_prompt(
            task,
            worktree,
            adb,
            {
                "head_before": head_before,
                "run_url": args.run_url,
                "comment_id": args.comment_id,
            },
        )
        prompt_path.write_text(prompt, encoding="utf-8")
        paseo_info = run_paseo(
            task,
            worktree,
            prompt,
            log,
            lock,
            state_dir / "logs" / f"{task.task_id}.paseo.log",
        )
        agent_id = paseo_info.get("agent_id")
        snap = git_snapshot(worktree)
        if task.is_inspect_only() and snap.get("diff_stat"):
            # dirty tree after inspect-only is not an automatic fail, but report it
            log.log("TEST", "inspect-only worktree has local diffs")
        pushed, push_note = maybe_push(task, worktree, log)
        result_md = read_result_file(worktree)
        agent_tail = collect_agent_logs(str(agent_id) if agent_id else None)
        paseo_exit = int(paseo_info.get("exit_code") or 1)
        status = "PASS" if paseo_exit == 0 else "FAIL"
        if paseo_exit != 0:
            failure_stage = "PASEO"
            error_summary = f"paseo run exited {paseo_exit}"
        sink.post(
            format_result(
                {
                    "task_id": task.task_id,
                    "status": status,
                    "repo": task.repo,
                    "branch": task.branch,
                    "head_before": head_before,
                    "head_after": snap.get("head"),
                    "runner": RUNNER_NAME,
                    "agent_id": agent_id,
                    "pushed": str(pushed).lower(),
                    "push_note": push_note,
                    "git_status": snap.get("status"),
                    "files_changed": snap.get("diff_stat") or "none",
                    "device": adb.get("summary"),
                    "worktree": str(worktree),
                    "local_log": str(state_dir / "logs" / f"{task.task_id}.log"),
                    "paseo_log": str(state_dir / "logs" / f"{task.task_id}.paseo.log"),
                    "agent_result": result_md,
                    "agent_tail": agent_tail,
                    "failure_stage": failure_stage,
                    "error_summary": error_summary,
                    "run_url": args.run_url,
                }
            )
        )
        log.log("DONE", "completed", status=status)
    except HeadMismatch as exc:
        status = "BLOCKED"
        failure_stage = "PREPARE"
        error_summary = f"expected HEAD mismatch: expected={task.expected_head} actual={exc.actual}"
        sink.post(
            format_result(
                {
                    "task_id": task.task_id,
                    "status": "BLOCKED",
                    "reason": "expected HEAD mismatch",
                    "expected_head": task.expected_head,
                    "actual_head": exc.actual,
                    "branch": task.branch,
                }
            )
        )
        log.log("DONE", error_summary, status=status)
    except TimeoutError as exc:
        status = "FAIL"
        failure_stage = "timeout"
        error_summary = str(exc)
        snap = git_snapshot(worktree) if worktree else {}
        sink.post(
            format_result(
                {
                    "task_id": task.task_id,
                    "status": "FAIL",
                    "failure_stage": "timeout",
                    "error_summary": error_summary,
                    "worktree": str(worktree) if worktree else "",
                    "git_status": snap.get("status"),
                    "local_log": str(state_dir / "logs" / f"{task.task_id}.log"),
                    "recommended_next": "inspect local logs and retry with a narrower task",
                }
            )
        )
        log.log("DONE", error_summary, status=status)
    except Exception as exc:  # noqa: BLE001 - orchestrator must always report
        status = "FAIL"
        failure_stage = failure_stage or "REPORT"
        error_summary = str(exc)
        snap = git_snapshot(worktree) if worktree else {}
        sink.post(
            format_result(
                {
                    "task_id": task.task_id,
                    "status": "FAIL",
                    "failure_stage": failure_stage or "unknown",
                    "error_summary": error_summary,
                    "worktree": str(worktree) if worktree else "",
                    "git_status": snap.get("status"),
                    "local_log": str(state_dir / "logs" / f"{task.task_id}.log"),
                    "recommended_next": "inspect local_log and worktree",
                }
            )
        )
        log.log("DONE", error_summary, status=status)
    finally:
        save_task_state(
            state_dir,
            task.task_id,
            {
                "status": status,
                "pid": os.getpid(),
                "issue": task.issue,
                "repo": task.repo,
                "branch": task.branch,
                "started_at": started,
                "finished_at": utc_now(),
                "agent_id": agent_id,
                "worktree": str(worktree) if worktree else "",
                "failure_stage": failure_stage,
                "error_summary": error_summary,
                "comment_id": args.comment_id,
            },
        )
        lock.release()
        log.log("LOCK", "released repository lock")
    return 0 if status == "PASS" else 1


def cmd_status(_args: argparse.Namespace) -> int:
    state_dir = home_state_dir()
    lock_path = state_dir / "locks" / f"{DEFAULT_REPO.replace('/', '-')}.lock"
    active = "none"
    extra: list[str] = []
    if lock_path.exists():
        try:
            data = json.loads(lock_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            data = {}
        pid = int(data.get("pid") or 0)
        if pid and _pid_alive(pid):
            active = str(data.get("task_id") or "unknown")
            extra = [
                f"pid: {pid}",
                f"started_at: {data.get('started_at')}",
                f"log: {state_dir / 'logs' / (str(data.get('task_id')) + '.log')}",
            ]
        else:
            extra = ["lock: stale"]
    adb = collect_adb()
    paseo = which("paseo")
    grok = which("grok")
    gh = which("gh")
    repo_ok = "unknown"
    try:
        probe = run_cmd(["gh", "api", f"repos/{DEFAULT_REPO}", "--jq", ".full_name"], timeout=20)
        repo_ok = "reachable" if probe.returncode == 0 else "unreachable"
    except Exception:  # noqa: BLE001
        repo_ok = "unreachable"
    github = "unauthenticated"
    if gh:
        auth = run_cmd(["gh", "auth", "status"], timeout=20)
        github = "authenticated" if auth.returncode == 0 else "unauthenticated"
    paseo_state = "missing"
    if paseo:
        ver = run_cmd([paseo, "--version"], timeout=20)
        daemon = run_cmd([paseo, "status", "--json"], timeout=20)
        ready = "available"
        if daemon.returncode == 0:
            try:
                payload = json.loads(daemon.stdout or "{}")
                if payload.get("localDaemon") != "running":
                    ready = "daemon-not-running"
            except json.JSONDecodeError:
                pass
        paseo_state = f"{ready} ({(ver.stdout or ver.stderr or '').strip().splitlines()[-1] if (ver.stdout or ver.stderr) else 'unknown'})"
    print(f"runner: {'ready' if active == 'none' else 'busy'}")
    print(f"active_task: {active}")
    for line in extra:
        print(line)
    print(f"adb: {adb['summary']}")
    print(f"repo: {repo_ok}")
    print(f"paseo: {paseo_state}")
    print(f"grok_build: {'available ' + grok if grok else 'missing'}")
    print(f"github: {github}")
    print(f"state_dir: {state_dir}")
    return 0


def cmd_cancel(args: argparse.Namespace) -> int:
    state_dir = home_state_dir()
    task_id = args.task_id
    state = load_task_state(state_dir, task_id) or {}
    lock_path = state_dir / "locks" / f"{DEFAULT_REPO.replace('/', '-')}.lock"
    lock_data = {}
    if lock_path.exists():
        try:
            lock_data = json.loads(lock_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            lock_data = {}
    if lock_data.get("task_id") and lock_data.get("task_id") != task_id:
        print(f"lock held by {lock_data.get('task_id')}, not {task_id}", file=sys.stderr)
        return 1
    agent_id = lock_data.get("agent_id") or state.get("agent_id")
    pgid = int(lock_data.get("paseo_pgid") or 0)
    pid = int(lock_data.get("pid") or state.get("pid") or 0)
    if agent_id and which("paseo"):
        run_cmd([which("paseo"), "stop", str(agent_id)], timeout=30)
    if pgid:
        _stop_pgid(pgid)
    elif pid and _pid_alive(pid) and pid != os.getpid():
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    print(f"cancel requested for {task_id}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="paseo-agent-runner")
    sub = parser.add_subparsers(dest="cmd")

    run_p = sub.add_parser("run", help="execute a task file")
    run_p.add_argument("--task", required=True, help="path to [PASEO_TASK v1] file")
    run_p.add_argument("--source", default="local")
    run_p.add_argument("--issue", type=int)
    run_p.add_argument("--comment-id")
    run_p.add_argument("--run-url")

    sub.add_parser("status", help="show dispatcher health")
    cancel_p = sub.add_parser("cancel", help="stop one task process group")
    cancel_p.add_argument("task_id")
    return parser


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    # Convenience: `paseo-agent-runner --task FILE` and `paseo-agent-runner status`
    if raw and raw[0] not in {"run", "status", "cancel", "-h", "--help"} and raw[0].startswith("-"):
        raw = ["run", *raw]
    parser = build_parser()
    args = parser.parse_args(raw)
    if args.cmd == "status":
        return cmd_status(args)
    if args.cmd == "cancel":
        return cmd_cancel(args)
    if args.cmd == "run":
        return dispatch(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
