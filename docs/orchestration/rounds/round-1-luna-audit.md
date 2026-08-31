# Round 1 Luna audit

## Result

Found and fixed a real cancelled-decode publication bug. Both BMP decode paths
wrote directly into the caller buffer while decoding row by row. If cancellation
was observed after a row had been written, they returned `false` but left a
partial asset in the output/publication arena, violating the decoder contract.

The decoder now uses the existing PSRAM-first allocator for bounded scratch
storage and commits to the caller buffer only after every row and a final
cancellation check succeed. Existing row/file cleanup remains on every failure
path.

The successful-file cancellation behavior in
`M4ProviderCoverCache::ensureSizedCoverFromSource` was inspected and intentionally
keeps a target generated before cancellation, so the next Home bind can use it.
That Lane A file was not changed.

## Coordinator follow-up (outside this lane's allowed files)

- `HomeActivity.cpp` can enter its legacy `Epub::generateThumbBmp` fallback
  after `ensureSizedCoverFromSource` returns due to cancellation, without a new
  cancellation check. This can write a cover after cancellation and should be
  handled by the coordinator.
- `HomeActivity.cpp` resets `backendCtx` while the display task reads the same
  `std::shared_ptr` and `renderSnapshotScene()` dereferences it in separate
  operations. This is an unsynchronized lifecycle race with a possible null
  dereference; the file is coordinator-owned and was not changed.
- The final cancellation check and `ctx.model.publish()` are not one atomic
  operation, so cancellation can race a post-cancel publication. The coordinator
  should decide whether to add a publication gate/join contract.

## Files changed

- `firmware/src/activities/home/HomeSceneAssetDecoder.cpp`
- `firmware/tests/native_app/test_home_lifecycle_uaf.cpp`
- `docs/orchestration/rounds/round-1-luna-audit.md`

No cover dimensions, Scene framework, Lane A files, simulator tests, or
coordinator worktree files were changed.

## Commands and results

- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py` — PASS.
- RED check before the production patch: focused lifecycle test failed on the
  new mid-decode no-write assertion.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src firmware/tests/native_app/test_home_lifecycle_uaf.cpp firmware/src/activities/home/HomeSceneAssetDecoder.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/m4_home_lifecycle_uaf_final && /tmp/m4_home_lifecycle_uaf_final` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src firmware/tests/native_app/test_home_decoder_hardening.cpp firmware/src/activities/home/HomeSceneAssetDecoder.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/m4_home_decoder_hardening_final && /tmp/m4_home_decoder_hardening_final` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src firmware/tests/native_app/test_home_asset_publication.cpp firmware/src/activities/home/HomeSceneAssetDecoder.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/m4_home_asset_publication_final && /tmp/m4_home_asset_publication_final` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src firmware/tests/native_app/test_home_scene_model.cpp firmware/src/ui/pages/HomeSceneModel.cpp -o /tmp/m4_home_scene_model_final && /tmp/m4_home_scene_model_final` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src firmware/tests/native_app/test_ui_state_store.cpp -o /tmp/m4_ui_state_store_final && /tmp/m4_ui_state_store_final` — PASS.
- `git diff --check` — PASS.
- AddressSanitizer was attempted with g++-14 but could not link because the
  environment lacks `-lasan`; no sanitizer result is claimed.

Full PlatformIO, QEMU, and hardware tests were intentionally not run per the
Round 1 task constraints.
