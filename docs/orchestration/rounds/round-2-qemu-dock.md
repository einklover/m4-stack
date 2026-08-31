# Round 2 QEMU dock + drawer evidence

Integration HEAD: `499fa66` (`orch: recrop dock icons from home mockup`).
Firmware: `202608187-murphy-m4-qemu-plugin`.
Flash sha256: `789b5bade4b3cbe21d3cfb4cbbb786f92f435b18dab7f85468cb90f0b9bc4e56`.
Session: `M4SIM_TMP=/tmp/m4sim-home-r2-dock` patched QEMU v3, `--plugin-debug --skip-build --no-net --keep-alive`.
No hardware flash. No `git push origin`.

## What landed

Dock is 文件管理 + 微信读书 + 番茄小说 + 晋江文学, icons recropped from `docs/orchestration/assets/home-mockup.jpg` dock row.
「更多」 tap `[401,601,72,20]` center `(437,611)` opens `AppList` 4-column drawer: 文件管理 / 阅读历史 / 网络管理 / 系统设置 then installed plugins.

## QEMU sequence

1. Stopped prior keep-alive with `./m4sim stop` (not `pkill`).
2. Reused SD (`murphy-sd.img`); did not `--fresh-sd`.
3. USB-installed WeRead 1.7.2 and Fanqie 1.3.3 (`noop: false`).
4. JJWXC USB install failed at ~66% (`SerialException` EIO); QEMU then SIGTERM.
5. After stop, `mcopy` replaced `/apps/com.jjwxc.client/icon_home.bmp` (hash `893eb901…` matches plugin BMP).
6. Rebooted skip-build. Home + tap 「更多」.

Physical PBM `800×480` converted with `/usr/bin/python3` PIL `ROTATE_270` to logical `480×800`.

## Shots (24h HTTP share)

- Home dock: http://179.255.101.123/share/-fH1V-BAfDLkLeYfcIZ8VA/home-jj-patched.png
- App drawer: http://179.255.101.123/share/FFT8ahNYO-e141whYtiWNw/drawer-499fa66.png
- Local: `/tmp/m4sim-home-r2-dock/shots/home-jj-patched.png`, `drawer-499fa66.png`

## Known leftovers (not dock blockers)

- Drawer 文件管理 uses `UIIcon::Folder` 4-gray `drawIcon`, not the compiled 1-bit dock folder.
- Recent-cover thumbnails on this SD seed are empty; labels still show.
- Optional drawer rows (OPDS / 坚果云 / 胶囊 / 书签) hidden because settings/bookmarks empty.
