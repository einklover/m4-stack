"""Centralized secret redaction for m4adb artifacts (serial logs, reports, timeline)."""

from __future__ import annotations

import re
from typing import Any

# Patterns that capture a secret-bearing prefix + secret body; replace body.
_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"(?i)(authorization\s*:\s*bearer\s+)(\S+)"), r"\1[REDACTED]"),
    (re.compile(r"(?i)(authorization\s*[:=]\s*)(\S+)"), r"\1[REDACTED]"),
    (re.compile(r"(?i)((?:set-)?cookie\s*[:=]\s*)([^\s;]+)"), r"\1[REDACTED]"),
    (re.compile(r"(?i)(password\s*[:=]\s*)(\S+)"), r"\1[REDACTED]"),
    (re.compile(r"(?i)(passwd\s*[:=]\s*)(\S+)"), r"\1[REDACTED]"),
    (re.compile(r"(?i)((?:api[_-]?key|access[_-]?token|refresh[_-]?token|id_token|token)\s*[:=]\s*)(\S+)"), r"\1[REDACTED]"),
    # WeRead / session-like cookie values in free text
    (re.compile(r"(?i)((?:wr_skey|wr_vid|session(?:id)?|sid)\s*[:=]\s*)([A-Za-z0-9._\-]{8,})"), r"\1[REDACTED]"),
    # URL query secrets
    (re.compile(r"([?&](?:password|passwd|token|access_token|refresh_token|api_key|key|secret|sid|session)=)([^&\s]+)", re.I), r"\1[REDACTED]"),
    # Bearer tokens bare
    (re.compile(r"(?i)(bearer\s+)([A-Za-z0-9._\-]{12,})"), r"\1[REDACTED]"),
    # Quoted JSON/log fields, e.g. {"refresh_token":"value"}.
    (
        re.compile(
            r'''(?i)(["'](?:authorization|cookie|set-cookie|password|passwd|api[_-]?key|access[_-]?token|refresh[_-]?token|id_token|token|secret|session(?:id)?|sid|wr_skey|wr_vid)["']\s*:\s*["'])([^"']*)(["'])'''
        ),
        r"\1[REDACTED]\3",
    ),
]

_SENSITIVE_KEY = re.compile(
    r"^(?:authorization|cookie|set-cookie|password|passwd|api[_-]?key|apikey|access[_-]?token|refresh[_-]?token|id_token|token|secret|session(?:id)?|sid|wr_skey|wr_vid)$",
    re.I,
)


def redact_text(text: str) -> str:
    if not text:
        return text
    out = text
    for pat, repl in _PATTERNS:
        out = pat.sub(repl, out)
    return out


def redact_obj(obj: Any) -> Any:
    """Deep-redact strings in dict/list structures."""
    if isinstance(obj, str):
        return redact_text(obj)
    if isinstance(obj, dict):
        return {
            k: "[REDACTED]" if _SENSITIVE_KEY.match(str(k)) else redact_obj(v)
            for k, v in obj.items()
        }
    if isinstance(obj, list):
        return [redact_obj(x) for x in obj]
    return obj
