#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
READER="$ROOT/firmware/src/activities/reader/TxtReaderActivity.cpp"
SESSION="$ROOT/firmware/src/activities/reader/TxtReaderActivity.h"
NATIVE="$ROOT/firmware/src/activities/apps/NativeProviderBookActivity.cpp"
fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q 'providerCoverBmpPath' "$SESSION" || fail "provider session lacks cached cover path"
grep -q 'sess.providerCoverBmpPath = providerCoverBmpPath_' "$NATIVE" \
  || fail "native provider does not pass cached cover into reader session"
grep -q 'pluginSession_.providerAuthor' "$READER" \
  || fail "first history creation lost provider author"
FIRST_ADD=$(grep -A1 'RECENT_BOOKS.addBook(uri, historyTitle, session.providerAuthor' "$READER" || true)
echo "$FIRST_ADD" | grep -q 'coverForRecents' \
  || fail "first history creation does not write the cached provider cover"
grep -q 'coverForRecents = session.providerCoverBmpPath' "$READER" \
  || fail "first history creation does not carry cached provider cover"
echo "first provider history metadata propagation passed"
