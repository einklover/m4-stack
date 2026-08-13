#!/usr/bin/env python3
"""Fetch JJWXC public androidapi responses to disk without whole-body buffering.

This helper is CI evidence plumbing for Issue #19. It preserves the plugin's
real request headers and stores only a temporary response file plus a compact
metadata summary. Callers should delete response bodies after streaming parser
verification and retain only hashes/counts/timings as artifacts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import ssl
import time
import urllib.error
import urllib.request
from pathlib import Path

API_HOST = "https://app-cdn.jjwxc.net"
APP_UA = (
    "Mozilla/5.0 (Linux; Android 5.1; Lenovo) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Version/4.0 Chrome/39.0.0.0 Mobile Safari/537.36/"
    "JINJIANG-Android/206(Lenovo;android 5.1;Scale/2.0)"
)
APP_REF = "http://android.jjwxc.net?v=206"


def request_headers() -> dict[str, str]:
    return {
        "User-Agent": APP_UA,
        "Referer": APP_REF,
        "Accept-Encoding": "identity",
        "Connection": "close",
    }


def stream_get(url: str, output: Path, timeout: float) -> dict:
    req = urllib.request.Request(url, headers=request_headers(), method="GET")
    output.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    total = 0
    started = time.monotonic()
    # The device loader currently uses WiFiClientSecure::setInsecure(); matching
    # that trust policy here avoids making host CI stricter than the production
    # path. HTTPS transport is still exercised end-to-end.
    ctx = ssl._create_unverified_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp, output.open("wb") as out:
            status = int(getattr(resp, "status", 0) or resp.getcode())
            while True:
                chunk = resp.read(8192)
                if not chunk:
                    break
                out.write(chunk)
                h.update(chunk)
                total += len(chunk)
            ctype = resp.headers.get("Content-Type", "")
            declared = resp.headers.get("Content-Length", "")
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        output.unlink(missing_ok=True)
        raise RuntimeError(f"live_fetch_failed: {exc}") from exc
    elapsed_ms = int((time.monotonic() - started) * 1000)
    if status != 200 or total < 2:
        output.unlink(missing_ok=True)
        raise RuntimeError(f"live_http_bad status={status} bytes={total}")
    return {
        "url": url,
        "http_status": status,
        "bytes": total,
        "sha256": h.hexdigest(),
        "elapsed_ms": elapsed_ms,
        "content_type": ctype,
        "declared_content_length": declared,
    }


def fetch_catalog(book_ids: list[str], output: Path, timeout: float) -> dict:
    errors: list[str] = []
    for book_id in book_ids:
        url = f"{API_HOST}/androidapi/chapterList?novelId={book_id}&more=0&whole=1"
        try:
            meta = stream_get(url, output, timeout)
            meta.update({"kind": "catalog", "book_id": book_id})
            return meta
        except RuntimeError as exc:
            errors.append(f"{book_id}:{exc}")
    raise RuntimeError("all_catalog_candidates_failed " + " | ".join(errors))


def fetch_chapter(book_id: str, chapter_id: str, output: Path, timeout: float) -> dict:
    url = f"{API_HOST}/androidapi/chapterContent?novelId={book_id}&chapterId={chapter_id}"
    meta = stream_get(url, output, timeout)
    meta.update({"kind": "chapter", "book_id": book_id, "chapter_id": chapter_id})
    return meta


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)

    cat = sub.add_parser("catalog")
    cat.add_argument("--book-id", action="append", dest="book_ids", required=True)
    cat.add_argument("--output", type=Path, required=True)
    cat.add_argument("--summary", type=Path, required=True)
    cat.add_argument("--timeout", type=float, default=45.0)

    ch = sub.add_parser("chapter")
    ch.add_argument("--book-id", required=True)
    ch.add_argument("--chapter-id", required=True)
    ch.add_argument("--output", type=Path, required=True)
    ch.add_argument("--summary", type=Path, required=True)
    ch.add_argument("--timeout", type=float, default=45.0)

    args = p.parse_args()
    try:
        if args.cmd == "catalog":
            meta = fetch_catalog(args.book_ids, args.output, args.timeout)
        else:
            meta = fetch_chapter(args.book_id, args.chapter_id, args.output, args.timeout)
    except RuntimeError as exc:
        print(str(exc))
        return 2

    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(meta, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
