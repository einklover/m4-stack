#!/usr/bin/env python3
"""Live WeRead protocol probe used to validate the M4x reader flow.

The script intentionally stores session cookies only under build/ (gitignored)
with mode 0600. It never prints cookie/token values.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import html
import json
import os
import random
import re
import stat
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import quote

import requests


HOST = "https://weread.qq.com"
UA = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 M4xWereadLiveProbe/1.0"
)


def md5_hex(data: str | bytes) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.md5(data).hexdigest()


def weread_e(value: str) -> str:
    digest = md5_hex(value)
    result = digest[:3]
    if value.isdigit() and value:
        type_flag = "3"
        chunks = [format(int(value[i : i + 9]), "x") for i in range(0, len(value), 9)]
    else:
        type_flag = "4"
        # Match firmware byteHex: %x, deliberately not zero padded.
        chunks = ["".join(format(b, "x") for b in value.encode("utf-8"))]
    result += type_flag + "2" + digest[-2:]
    for index, chunk in enumerate(chunks):
        result += f"{len(chunk):02x}" + chunk
        if index + 1 < len(chunks):
            result += "g"
    while len(result) < 20:
        result += digest[: 20 - len(result)]
    return result + md5_hex(result)[:3]


def weread_sign(query: str) -> str:
    a = 0x15051505
    b = a
    length = len(query)
    i = length
    raw = query.encode("utf-8")
    # Production query keys/encoded values are ASCII, matching C++ byte indexing.
    while i > 1:
        a = (a ^ (raw[i - 1] << ((length - i + 1) % 30))) & 0x7FFFFFFF
        b = (b ^ (raw[i - 2] << ((i - 1) % 30))) & 0x7FFFFFFF
        i -= 2
    return format(a + b, "x").lower()


def make_content_params(book_id: str, chapter_uid: str, psvts: str) -> dict[str, Any]:
    ct = int(time.time())
    if weread_e(str(ct)) == psvts:
        ct += 1
    rnd = random.randint(0, 9999)
    params: dict[str, str] = {
        "b": weread_e(book_id),
        "c": weread_e(chapter_uid),
        "r": str(rnd * rnd),
        "ct": str(ct),
        "ps": psvts,
        "pc": weread_e(str(ct)),
        "sc": "1",
        "prevChapter": "false",
        "st": "0",
    }
    query = "&".join(
        f"{key}={quote(value, safe='-_.~')}" for key, value in sorted(params.items())
    )
    params["s"] = weread_sign(query)
    return {
        key: int(value) if key in {"ct", "sc", "st", "r"} else value
        for key, value in params.items()
    }


def checked_body(shard: str) -> str:
    if len(shard) <= 32:
        return ""
    expected = shard[:32]
    body = shard[32:]
    return body if md5_hex(body).upper() == expected else ""


def swap_positions(encoded: bytearray) -> list[int]:
    length = len(encoded)
    if length < 4:
        return []
    if length < 11:
        return [0, 2]
    count = min(4, (length + 9) // 10)
    tail = ""
    for index in range(length - 1, length - count - 1, -1):
        value = encoded[index]
        expanded = sum(1 << (2 * bit) for bit in range(8) if (value >> bit) & 1)
        tail += str(expanded)
    modulo = length - count - 2
    if modulo <= 0:
        return []
    step = len(str(modulo))
    positions: list[int] = []
    index = 0
    while len(positions) < 10 and index + step < len(tail):
        positions.append(int(tail[index : index + step]) % modulo)
        positions.append(int(tail[index + 1 : index + 1 + step]) % modulo)
        index += step
    return positions


def decode_shards(*shards: str) -> bytes:
    payload = "".join(checked_body(shard) for shard in shards)
    if len(payload) < 2:
        return b""
    encoded = bytearray(payload[1:].encode("ascii", errors="ignore"))
    positions = swap_positions(encoded)
    for index in range(len(positions) - 1, 0, -2):
        for offset in (1, 0):
            left = positions[index] + offset
            right = positions[index - 1] + offset
            if left < len(encoded) and right < len(encoded):
                encoded[left], encoded[right] = encoded[right], encoded[left]
    clean = bytes(
        ch
        for ch in encoded
        if chr(ch).isalnum() or chr(ch) in "-_+/"
    ).replace(b"-", b"+").replace(b"_", b"/")
    clean += b"=" * ((4 - len(clean) % 4) % 4)
    try:
        return base64.b64decode(clean, validate=False)
    except Exception:
        return b""


def first_psvts(page: str) -> str:
    match = re.search(r'"psvts"\s*:\s*"([^"]+)"', page)
    return match.group(1) if match else ""


def write_private_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    path.chmod(stat.S_IRUSR | stat.S_IWUSR)


def response_json(response: requests.Response, label: str) -> Any:
    try:
        return response.json()
    except Exception as exc:
        raise RuntimeError(f"{label}: HTTP {response.status_code}, invalid JSON") from exc


def login(session: requests.Session, out_dir: Path, timeout_seconds: int) -> None:
    response = session.get(f"{HOST}/api/auth/getLoginUid", timeout=15)
    response.raise_for_status()
    uid = str(response_json(response, "getLoginUid").get("uid", ""))
    if not uid:
        raise RuntimeError("getLoginUid returned no uid")
    login_url = f"{HOST}/web/confirm?uid={uid}"
    try:
        import qrcode  # type: ignore
    except ImportError as exc:
        raise RuntimeError("missing qrcode package; install qrcode[pil]") from exc
    qr_path = out_dir / "weread_login_qr.png"
    qrcode.make(login_url).save(qr_path)
    print(f"QR_PATH={qr_path}", flush=True)
    print("WAITING_FOR_SCAN", flush=True)

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            poll = session.get(
                f"{HOST}/api/auth/getLoginInfo",
                params={"uid": uid, "otp": ""},
                timeout=15,
            )
        except requests.RequestException:
            # A transient server/DNS/read timeout must not invalidate the QR
            # or abort a user-mediated login session.
            time.sleep(2)
            continue
        if poll.ok:
            data_doc = response_json(poll, "getLoginInfo")
            data = data_doc.get("data") or data_doc
            if data.get("succeed"):
                vid = str(data.get("webLoginVid") or data.get("vid") or "")
                token = str(data.get("accessToken") or "")
                if vid and not session.cookies.get("wr_vid"):
                    session.cookies.set("wr_vid", vid, domain="weread.qq.com", path="/")
                if token and not session.cookies.get("wr_skey"):
                    session.cookies.set("wr_skey", token, domain="weread.qq.com", path="/")
                if session.cookies.get("wr_vid") and session.cookies.get("wr_skey"):
                    print("LOGIN_OK", flush=True)
                    return
        time.sleep(2)
    raise TimeoutError("scan/login timed out")


def choose_book_and_chapter(session: requests.Session) -> tuple[dict[str, Any], dict[str, Any]]:
    shelf_response = session.get(f"{HOST}/web/shelf/sync", timeout=30)
    shelf_response.raise_for_status()
    shelf = response_json(shelf_response, "shelf")
    books = shelf.get("books") or []
    if not books:
        raise RuntimeError("shelf is empty")
    progress_by_id = {
        str(item.get("bookId")): item for item in (shelf.get("bookProgress") or [])
    }
    normalized: list[dict[str, Any]] = []
    for item in books:
        book = item.get("book") or item
        book_id = str(book.get("bookId") or item.get("bookId") or "")
        if not book_id:
            continue
        progress = progress_by_id.get(book_id, {})
        normalized.append(
            {
                "bookId": book_id,
                "title": str(book.get("title") or item.get("title") or ""),
                "updated": int(progress.get("readUpdateTime") or progress.get("updateTime") or 0),
                "progressChapterUid": str(progress.get("chapterUid") or ""),
            }
        )
    normalized.sort(key=lambda book: book["updated"], reverse=True)
    for book in normalized:
        toc_response = session.post(
            f"{HOST}/web/book/chapterInfos",
            json={"bookIds": [book["bookId"]]},
            timeout=30,
        )
        if not toc_response.ok:
            continue
        toc = response_json(toc_response, "chapterInfos")
        data = toc.get("data") or []
        chosen = next(
            (
                item
                for item in data
                if str(item.get("bookId") or (item.get("book") or {}).get("bookId") or "")
                == book["bookId"]
            ),
            data[0] if data else None,
        )
        if not chosen:
            continue
        chapters = chosen.get("updated") or chosen.get("chapterInfos") or []
        chapters = [chapter for chapter in chapters if chapter.get("chapterUid") is not None]
        if not chapters:
            continue
        preferred = book["progressChapterUid"]
        chapter = next(
            (chapter for chapter in chapters if str(chapter.get("chapterUid")) == preferred),
            chapters[0],
        )
        return book, {
            "chapterUid": str(chapter.get("chapterUid")),
            "title": str(chapter.get("title") or ""),
        }
    raise RuntimeError("no readable book/chapter found")


def probe_chapter(
    session: requests.Session, book: dict[str, Any], chapter: dict[str, Any], out_dir: Path
) -> None:
    encoded_book = weread_e(book["bookId"])
    encoded_chapter = weread_e(chapter["chapterUid"])
    reader_url = f"{HOST}/web/reader/{encoded_book}k{encoded_chapter}"
    page_response = session.get(reader_url, timeout=30)
    page_path = out_dir / "reader_response.html"
    page_path.write_text(page_response.text, encoding="utf-8")
    psvts = first_psvts(page_response.text)
    print(
        f"READER status={page_response.status_code} bytes={len(page_response.content)} "
        f"psvts={'yes' if psvts else 'no'}",
        flush=True,
    )
    if not page_response.ok or not psvts:
        title = re.search(r"<title>(.*?)</title>", page_response.text, re.I | re.S)
        title_text = html.unescape(title.group(1).strip()) if title else ""
        raise RuntimeError(
            f"reader page unusable: HTTP {page_response.status_code}, title={title_text!r}"
        )

    referer = reader_url
    params = make_content_params(book["bookId"], chapter["chapterUid"], psvts)
    shards: dict[str, str] = {}
    for endpoint in ("e_0", "e_1", "e_3"):
        response = session.post(
            f"{HOST}/web/book/chapter/{endpoint}",
            json=params,
            headers={"Referer": referer},
            timeout=30,
        )
        shards[endpoint] = response.text
        print(f"{endpoint} status={response.status_code} bytes={len(response.content)}", flush=True)
        if not response.ok:
            raise RuntimeError(f"{endpoint}: HTTP {response.status_code}")
    decoded = decode_shards(shards["e_0"], shards["e_1"], shards["e_3"])
    if not decoded:
        raise RuntimeError("EPUB shard decode failed")
    text_path = out_dir / "chapter_decoded.xhtml"
    text_path.write_bytes(decoded)
    print(f"DECODE_OK bytes={len(decoded)} path={text_path}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="build/weread_live_probe")
    parser.add_argument("--login-timeout", type=int, default=180)
    args = parser.parse_args()
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    session = requests.Session()
    session.headers.update({"User-Agent": UA, "Referer": f"{HOST}/"})
    login(session, out_dir, args.login_timeout)
    safe_cookies = {
        name: value
        for name, value in session.cookies.get_dict().items()
        if name in {"wr_vid", "wr_skey", "wr_rt"}
    }
    write_private_json(out_dir / "session.json", {"cookies": safe_cookies})
    book, chapter = choose_book_and_chapter(session)
    print(
        f"SELECTED book={book['title']!r} chapter={chapter['title']!r}",
        flush=True,
    )
    probe_chapter(session, book, chapter, out_dir)
    print("PROBE_OK", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"PROBE_FAILED {type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
