#!/usr/bin/env bash
# One-shot APP1 flash + OTA slot switch + daemon restart (no "first flash does
# nothing" confusion). The flash helper's internal otatool call always fails
# (anaconda python lacks esptool) — that is expected; we suppress it and do the
# slot switch ourselves with the penv python. Never re-run the whole flash.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-/dev/cu.usbmodem101}"
IDF_PATH="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}"
PENV_PY="$HOME/.platformio/penv/bin/python"
OTATOOL="$IDF_PATH/components/app_update/otatool.py"

echo "[flash] stop m4adb (single owner cleanup)"
ps -axo pid=,command= | awk '/[m]4adb\.py/ {print $1}' > /tmp/m4_pids.txt 2>/dev/null || true
if [ -s /tmp/m4_pids.txt ]; then xargs kill < /tmp/m4_pids.txt 2>/dev/null || true; fi
sleep 2
rm -f /tmp/m4adb-*.sock

echo "[flash] write APP1 (otatool inside script is expected to fail — ignored)"
OUT=$(IDF_PATH="$IDF_PATH" python3 "$ROOT/scripts/murphy_m4_app1_flash.py" \
  --port "$PORT" --i-understand-app1-only --skip-backup 2>&1 || true)
if ! echo "$OUT" | grep -q "Hash of data verified"; then
  echo "[flash] ERROR: firmware write did not verify:"; echo "$OUT" | tail -5
  exit 1
fi
echo "[flash] APP1 written + hash verified"

echo "[flash] switch OTA slot 1 (penv python + IDF_PATH + PYTHONPATH)"
SLOT=$(IDF_PATH="$IDF_PATH" PYTHONPATH="$IDF_PATH/components/partition_table" \
  "$PENV_PY" "$OTATOOL" --port "$PORT" switch_ota_partition --slot 1 2>&1 | grep -c "verified" || true)
if [ "$SLOT" -ge 1 ]; then echo "[flash] OTA slot 1 selected"; else echo "[flash] WARN: slot switch unverified"; fi

echo "[flash] wait for device reboot"
sleep 10
nohup "$PENV_PY" "$ROOT/scripts/m4adb.py" daemon --ready-timeout 60 > "$ROOT/build/m4adb/daemon.log" 2>&1 &
sleep 8
# First status may race device boot / daemon — retry, never re-flash.
for i in 1 2 3; do
  R=$("$PENV_PY" "$ROOT/scripts/m4adb.py" status 2>&1 | grep -E '"activity"|"free_heap"')
  if [ -n "$R" ]; then echo "[flash] device ready (attempt $i): $R"; exit 0; fi
  sleep 5
done
echo "[flash] WARN: status not ready after retries — device may still be booting"
exit 0
