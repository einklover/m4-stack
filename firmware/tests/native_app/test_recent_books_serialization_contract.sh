#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STORE="$ROOT/firmware/src/RecentBooksStore.cpp"
HEADER="$ROOT/firmware/src/RecentBooksStore.h"
fail() { echo "FAIL: $1" >&2; exit 1; }

# Track E deliberately keeps the version and five serialized strings intact.
grep -q 'RECENT_BOOKS_FILE_VERSION = 4' "$STORE" || fail "RecentBooks version changed"
for field in path title author coverBmpPath originalSourcePath; do
  grep -q "writeString(outputFile, book\\.$field)" "$STORE" || fail "missing write for $field"
  grep -q "serialization::readString(inputFile, $field)" "$STORE" || fail "missing read for $field"
done
grep -q 'updateProviderBook' "$HEADER" || fail "missing provider metadata merge API"
echo "recent books serialization compatibility passed"
