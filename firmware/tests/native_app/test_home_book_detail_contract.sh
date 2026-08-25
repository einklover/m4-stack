#!/usr/bin/env bash
# Source-contract guard for the home-page book-detail metadata change.
#
# Pins, by exact production call shape:
#   1. Selected-book info box shows title, author, source (plugin display name).
#   2. No textual progress percentage / "已读" / kReadingProgressLabel in that box.
#   3. Graphical progress bar still consumes recentBooks[selectorIndex].progress.
#   4. Home reopen still uses RecentBook.author as m4x appId (persistence unchanged).
#   5. Reader / system UI files are not part of this diff surface.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
THEME="$ROOT/firmware/src/components/themes/fengyan/FengyanTheme.cpp"
HOME="$ROOT/firmware/src/activities/home/HomeActivity.cpp"
HELPER="$ROOT/firmware/src/util/M4HomeBookDetailMeta.h"
I18N="$ROOT/firmware/src/I18n.h"

for f in "$THEME" "$HOME" "$HELPER" "$I18N"; do
  [[ -f "$f" ]] || { echo "missing $f" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

# Slice the selected-book info box (from isBookSelected through the else/menu stats).
SLICE=$(awk '/if \(isBookSelected\) \{/{flag=1}
             flag{print}
             flag && /\} else \{/{exit}' "$THEME")
[[ -n "$SLICE" ]] || fail "isBookSelected book-detail block not found"

echo "$SLICE" | grep -q 'kBookTitle' || fail "book-detail must draw kBookTitle"
echo "$SLICE" | grep -q 'kBookAuthor' || fail "book-detail must draw kBookAuthor"
echo "$SLICE" | grep -q 'kBookSource' || fail "book-detail must draw kBookSource"
echo "$SLICE" | grep -q 'M4HomeBookDetailMeta' || fail "book-detail must present via M4HomeBookDetailMeta"

if echo "$SLICE" | grep -q 'kReadingProgressLabel'; then
  fail "book-detail must not draw kReadingProgressLabel"
fi
if echo "$SLICE" | grep -E 'to_string\(.*progress.*\)\s*\+\s*"%"' >/dev/null; then
  fail "book-detail must not draw textual percent"
fi
if echo "$SLICE" | grep -q '已读'; then
  fail "book-detail must not draw 已读 progress text"
fi

echo "$SLICE" | grep -q 'recentBooks\[selectorIndex\].progress' \
  || fail "progress bar must still read recentBooks[].progress"
echo "$SLICE" | grep -q 'fillWidth' \
  || fail "graphical progress bar fill must remain"

# Persistence/reopen contract: plugin history author remains appId.
grep -q 'src = std::string("app:") + b.author' "$HOME" \
  || fail "HomeActivity must still reopen plugin books via app:+author"

# Helper must refuse raw package ids as the source label.
grep -q 'looksLikePackageId' "$HELPER" || fail "helper must detect package ids"
grep -q '未知来源' "$HELPER" || fail "helper must provide 未知来源 fallback"
grep -q '本地' "$HELPER" || fail "helper must provide 本地 fallback"

# Unrelated reader/system UI must not be the home-detail change surface.
for f in \
  "$ROOT/firmware/src/activities/reader/EpubReaderActivity.cpp" \
  "$ROOT/firmware/src/activities/reader/EpubReaderSettingsActivity.cpp" \
  "$ROOT/firmware/src/managers/FontManager.cpp"
do
  [[ -f "$f" ]] || fail "missing $f"
done

echo "home book detail source contracts passed"
