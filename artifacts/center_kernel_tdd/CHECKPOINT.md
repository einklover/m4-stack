# Center-kernel WIP checkpoint (supervisor freeze)

- Integration HEAD: f07efb8e1e2a5fec4c8c5b5e0ccb8f8331f08303
- Branch: agent/m4-ox-supervisor-integration
- Checkpoint branch: agent/m4-ck-wip-checkpoint
- Unrelated dirty native-grid/theme/tab-chip files were NOT included.

## Validated RED (do not re-litigate)
`python3 simulator/tests/test_center_kernel_reader_contract.py` → 9 FAIL / 1 PASS (chrome preservation).

## Files in this checkpoint
- simulator/tests/test_center_kernel_reader_contract.py (source-contract tests; some substring assertions are strict)
- artifacts/center_kernel_tdd/stash/CenterKernelFont.h (math reference ONLY; not yet in production path)
- artifacts/center_kernel_tdd/stash/generate_m4_center_kernel.py (TTF generator REFERENCE; last-resort only)
- artifacts/center_kernel_tdd/RED.log and RED_rerun.log

## Occupancy facts (OX-A)
- Candidate occupancy.txt is 7 representative glyphs, NOT 27553 grids.
  /Volumes/z/paseo/workspaces/paseo/worktrees/0xdf4ldr/m4-integrated-candidate/artifacts/pixel_center_absolute_occupancy.txt
- Report has 27553 joint_metric_class_id records but NO occupancy bitmaps.
  /Volumes/z/paseo/workspaces/paseo/worktrees/0xdf4ldr/m4-integrated-candidate/artifacts/pixel_center_absolute_report.json
- Lattice report is LSB-normalized 15-col; DO NOT use as stored occupancy.
- Do NOT install fontTools as a firmware-build dependency.
- Do NOT add a generic --from-occupancy CLI to firmware build.
- TTF re-extract is last resort only if occupancy bits cannot be produced from existing artifacts.

## Hard rules for all OX workers
- Isolated worktree only. Do not edit supervisor or sibling OX worktrees.
- Chrome/system UI stays on existing NativeGridEpdFont path.
- Non-CJK/Latin/punct fallback unchanged; CJK corpus only (U+3400–U+9FFF, 27553).
- No neighbor-derived kernel heuristics. Use joint geometry class table.
- No Android ADB, no QEMU flash, no pkill m4adb, no APP0/bootloader/full erase.
- Do not modify m4-integrated-candidate.
- rhu(x)=floor(x+0.5). Never Python round() / banker's rounding.
- Allowed reader BODY sizes exactly {16,24,26,36,38,40,48}; default 26; no 32/45.
