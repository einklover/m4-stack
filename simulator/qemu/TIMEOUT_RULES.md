# m4-stack QEMU / m4adb timeout rules (agent)

## Do NOT
- sleep 50–70s hoping discovery finishes
- poll m4adb for minutes when ping already timed out
- use m4adb right after "bridge ready" (wifi_status still holds PTY)

## Do
- Wait for `keep-alive` / `Plugin debug session` before using PTY
- Per command: wall-clock kill via `simulator/qemu/m4_quick.py` or `subprocess timeout=`
- Explicit short: `m4adb --timeout 5 --ready-timeout 5 --no-daemon ping`
- Overall fanqie smoke budget ≤ 45s: `python3 simulator/qemu/m4_quick.py fanqie`
- First frozen ping after launch → stop, do not retry 10×

## Root causes of long waits
1. Guest freeze during native discovery HTTPS (QEMU)
2. m4adb old ready floor forced ≥15s even with --timeout 6
3. Tool default shell timeout 120s × stacked commands
