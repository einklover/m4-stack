# M4 SD and Registry integration: 2026-09-27

Branch: feature/m4-stability-integration-20260927. Base SD fix f98afb0; Registry cherry-pick ff4317e (from 8d82504). No origin push or main merge.

## Verified tests
- Five merged-candidate host tests exit 0: SD guard scope, FsCache interleave, SD retry, Registry journal, Registry transaction-lock boundary. git diff --check exit 0.
- PIO murphy_m4 exit 0: RAM 109028/327680, flash 5783213/7143424. PIO murphy_m4_qemu_plugin exit 0.

## Isolated QEMU
- Fresh isolated 64 MiB FAT32, --no-net --no-hostfwd. Boot/stop, status, font list, and sd_probe all exit 0. SD open/write/sync/read/delete all true.
- Five prebuilt .m4x packages (bulls, connect, sliding, sokoban, nonogram) each passed USB install, launch, direction key, back and Home status with SD OK, exit 0.
- Final free_heap 156828, min_free_heap 146432, free_psram 1769116. QEMU log has a timer-class warning but no firmware panic text; reset_reason=4 remains unattributed.

## Review and remaining gaps
- Astra read-only review of ff4317e found no confirmed production blocker in this scope; local report: /tmp/m4-astra-integration-review-ff4317e-20260927.md.
- Not done: real FAT/exFAT two-task post-helper barrier; deterministic concurrent Registry save/load; counted-device SD init failure injection; real-device SD, multi-TTF/reader/plugin stress.
- WebSocket cross-task mutex and JPEG allocation issues from the broader audit remain separate. Game source branch is not merged here; only its prebuilt packages were installed on isolated QEMU.
