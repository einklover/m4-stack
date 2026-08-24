#!/usr/bin/env bash
# Source-contract guard for the reader settings handoff fixes
# (agent/fix-reader-settings-handoff).
#
# Pins, by exact production call shape:
#   1. EpubReaderActivity pumps children only via pumpSubActivityFrame(), and a
#      full child exit keeps/forces updateRequired=true (no blind clobber).
#   2. The EPUB settings onGoBack re-arms the auto page turn timer.
#   3. Menu/settings pump nested children via pumpSubActivityFrame(); no direct
#      subActivity->loop() remains in either activity.
set -euo pipefail

EPUB=firmware/src/activities/reader/EpubReaderActivity.cpp
MENU=firmware/src/activities/reader/EpubReaderMenuActivity.cpp
SETTINGS=firmware/src/activities/reader/EpubReaderSettingsActivity.cpp

for f in "$EPUB" "$MENU" "$SETTINGS"; do
  [[ -f "$f" ]] || { echo "missing $f" >&2; exit 1; }
done

fail() { echo "FAIL: $1" >&2; exit 1; }

# ── Contract 1: no unconditional repaint clobber after pumpSubActivityFrame.
grep -q 'pumpSubActivityFrame' "$EPUB" || fail "EpubReaderActivity must pump subactivity frames"

# Slice the post-pump transition handler (up to the deferred-exit block).
SLICE=$(awk '/const bool replaced = pumpSubActivityFrame\(\);/{flag=1}
             flag{print}
             flag && /pendingSubactivityExit/{exit}' "$EPUB")
[[ -n "$SLICE" ]] || fail "pumpSubActivityFrame transition handler not found"

# A full child EXIT (no replacement) must keep/force the reader repaint request.
echo "$SLICE" | grep -q 'updateRequired = true;' \
  || fail "child-exit branch must set updateRequired = true (repaint request)"

# Any clear inside the handler must sit under the replacement-child guard,
# never as a bare statement (that is the audit's blind-clobber bug).
if echo "$SLICE" | grep -q 'updateRequired = false;'; then
  echo "$SLICE" | grep -B3 'updateRequired = false;' | grep -q 'if (subActivity)' \
    || fail "unconditional updateRequired=false after pump (blind clobber restored)"
fi

# ── Contract 2: settings onGoBack re-arms auto page turn.
SETTINGS_CB=$(grep -A9 'enterNewActivity(new EpubReaderSettingsActivity(' "$EPUB" || true)
echo "$SETTINGS_CB" | grep -q 'applyAutoPageTurnSettings();' \
  || fail "EpubReaderSettingsActivity onGoBack must re-arm applyAutoPageTurnSettings()"
echo "$SETTINGS_CB" | grep -q 'updateRequired = true;' \
  || fail "EpubReaderSettingsActivity onGoBack must request a reader repaint"

# ── Contract 3: menu/settings defer nested teardown via pumpSubActivityFrame.
for f in "$MENU" "$SETTINGS"; do
  if grep -nE 'subActivity->loop\(\)' "$f" >/dev/null 2>&1; then
    fail "$(basename "$f") calls subActivity->loop() directly (inline nested destruction)"
  fi
  grep -q 'pumpSubActivityFrame()' "$f" \
    || fail "$(basename "$f") must route nested children through pumpSubActivityFrame()"
done

echo "reader settings handoff source contracts passed"
