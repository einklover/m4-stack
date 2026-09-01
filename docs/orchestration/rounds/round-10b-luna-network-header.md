# Round 10b — CrossPoint network route + stronger header air

Date: 2026-09-01
Branch: `agent/home-luna-audit`
Base: `9ebda92` (`orch: merge luna-audit round-10 AppList routes + header inset`)

## Result

- `网络管理` now reuses `onGoToFileTransfer()`, which enters the existing
  `CrossPointWebServerActivity` three-method transfer chooser.
- `文件管理` remains wired to `onGoToMyLibrary()` and lands in
  `MyLibraryActivity`.
- `HomeRef::HeaderSafeTop` is now 20 px. The Home scene keeps its generated
  `HomeHeader*` geometry and was not changed.

## Root cause

Round 10 sent `网络管理` to Settings `网络与同步` L2. That was a product
destination miss: the label is expected to open the same CrossPoint chooser
used by the old FileTransfer path. The route now delegates to the canonical
transfer entry, so there is one activity construction path for both entries.

The Round 10 safe-top value of 12 px moved the AppList title down, but its
measured QEMU first ink was still only about y=19. With the same non-Home
baseline and `FengyanTheme::drawHeader` offset logic, 20 px provides the
requested optical air without touching the Home reference scene.

## QEMU evidence

Firmware was built from this worktree with the interactive
`murphy_m4_qemu_plugin` profile, then run once with a fresh SD image:

```text
M4SIM_TMP=/tmp/m4sim-round10b-luna ./m4sim run \
  --plugin-debug --skip-build --fresh-sd --no-net --no-hostfwd \
  --ready-seconds 20 --keep-alive
```

Logical framebuffer is 480×800. Journey and status results:

```text
Home                 -- tap (437,611) --> AppList
AppList              -- tap (390,140) --> CrossPointWebServer
AppList              -- tap (90,140)  --> MyLibrary
```

The `CrossPointWebServer` frame visibly contains the three transfer methods:
手机连接到设备传书、设备连接到 WiFi 传书、使用Calibre无线设备传输.

Title measurement used the fresh AppList RGB frame and the title region
`x=55..304, y=0..79`: first dark pixel is **y=27**. The fresh Home frame's
full top scan starts at **y=24**, confirming the Home scene remains in its
normal top band.

Fresh logical captures (RGB PNG, 480×800):

- Home: `tmp-home-screenshots/round-10b-luna/00-home-logical.png`
  ([temporary URL](http://179.255.101.123/share/v1aJ6MjborVRN8z-aIqK5A/00-home-logical.png))
- AppList, title y=27: `tmp-home-screenshots/round-10b-luna/01-applist-logical.png`
  ([temporary URL](http://179.255.101.123/share/E-tHXq_hMQEBZg8yMphxrQ/01-applist-logical.png))
- Network → CrossPoint chooser: `tmp-home-screenshots/round-10b-luna/02-network-crosspoint-logical.png`
  ([temporary URL](http://179.255.101.123/share/smLGEPzEOS1xz6mhUEfuXg/02-network-crosspoint-logical.png))
- File manager → MyLibrary: `tmp-home-screenshots/round-10b-luna/03-file-manager-mylibrary-logical.png`
  ([temporary URL](http://179.255.101.123/share/Cfq2YZeokT5h0t2eKMOOMg/03-file-manager-mylibrary-logical.png))

The PNG SHA-256 values are, respectively:

```text
00-home-logical.png                       2b9a774f3fcff82367b438f5423411732a42587a18da2584fa429954d631bda2
01-applist-logical.png                    71d410def99223370af7184928b42163848be62abbd5aa51fd8f950acc17cd9e
02-network-crosspoint-logical.png         4ae654c8418aa2ac5f6857d89c6a407e64ef1f2fd4ec317dcf090b09d4be11a2
03-file-manager-mylibrary-logical.png     c92ecfe63785fc3e0d8909dd0c75fea89821c5151832772a36697254ad9ad21a
```

## Verification

Focused host contracts:

```text
/opt/anaconda3/bin/pytest -q \
  firmware/tests/test_round10_applist_routes_header.py \
  firmware/tests/test_app_drawer_3col.py \
  firmware/tests/test_home_app_drawer.py \
  firmware/tests/test_m4_dependency_bootstrap_contract.py
14 passed
```

Builds:

```text
PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
  /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4_qemu_plugin -j1
SUCCESS — RAM 32.8%, Flash 75.0%

PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
  /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1
SUCCESS — RAM 33.0%, Flash 74.9%
```

No device was flashed, no origin push was performed, and unrelated dirty files
were preserved.
