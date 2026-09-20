#!/usr/bin/env bash
# One-shot APP1 flash + OTA slot switch + one daemon. Uses PlatformIO python
# (anaconda has no esptool). Never re-run the whole flash on a slot-only miss.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-/dev/cu.usbmodem101}"
IDF_PATH="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}"
PENV_PY="$HOME/.platformio/penv/bin/python"

echo "[flash] stop m4adb via daemon_stop (single owner)"
"$PENV_PY" "$ROOT/scripts/m4adb.py" --port "$PORT" daemon_stop >/dev/null 2>&1 || true
sleep 2

echo "[flash] write APP1 + switch slot 1 (penv python, USB-JTAG default baud, after no-reset)"
set +e
OUT=$(IDF_PATH="$IDF_PATH" "$PENV_PY" "$ROOT/scripts/murphy_m4_app1_flash.py" \
  --port "$PORT" --i-understand-app1-only --skip-backup 2>&1)
RC=$?
set -e
echo "$OUT"
if ! echo "$OUT" | grep -q "Hash of data verified"; then
  echo "[flash] ERROR: firmware write did not verify"
  exit 1
fi
if [ "$RC" -ne 0 ]; then
  echo "[flash] ERROR: helper exited $RC after verified write — do not rewrite APP1; retry slot only"
  exit 1
fi
echo "[flash] APP1 written + OTA slot selected"

echo "[flash] wait for device reboot"
sleep 10
mkdir -p "$ROOT/build/m4adb"
nohup "$PENV_PY" "$ROOT/scripts/m4adb.py" daemon --ready-timeout 60 > "$ROOT/build/m4adb/daemon.log" 2>&1 &
sleep 8
for i in 1 2 3; do
  R=$("$PENV_PY" "$ROOT/scripts/m4adb.py" status 2>&1 | grep -E '"activity"|"free_heap"')
  if [ -n "$R" ]; then echo "[flash] device ready (attempt $i): $R"; exit 0; fi
  sleep 5
done
echo "[flash] WARN: status not ready after retries — device may still be booting"
exit 0
