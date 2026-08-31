# Round 5 — Muse plugin-icon RCA (drawer + Home dock)

## Symptom (human correction, authoritative)

Not "blank/white tiles". On device (and on QEMU at `m4-critical-ui-home` `2d28b77`):

* **Icon slot (62×64) shows two horizontal bars** — top and bottom edges of the 62×64, full-width, 1–2 px.
* **Version string between the bars**, centered, small UI_10-like font.
* **Only 晋江 `com.jjwxc.client` shows the version** (observed `v1.4.2` on old QEMU; human reports `v1.2.3` on current device — same class). `com.weread.client` / `com.fanqie.client` show the two-bar motif with empty middle (weaker).
* **Labels under tiles are fine** (`晋江文学` / `番茄小说` / `微信读书` correctly truncated). **Builtin `文件管理` (folder) icon is fine** on both surfaces. **Same class on Home dock** (files slot 0 OK, three plugin slots wrong).

This is a content error in the 62×64 icon bitmap, not an empty tile.

Evidence (this lane):

* QEMU `tip 485b7c7` + manual `mcopy` SD (`/tmp/m4sim/artifacts/murphy-sd.img`, 3 plugins, 574 B `icon_home.bmp` each, `app_registry.json` correct) → **correct** line icons: drawer row `J` / tomato / WeChat (≈700–809 black / 3968, 18–20 %) and Home dock folder OK — no bars. See `/tmp/m4-round4-evidence/qemu-drawer-plugins-qemu.png` and `/tmp/qemu-retest-drawer-plugins.png`.
* Device-like QEMU `m4-critical-ui-home` `2d28b77` (`tmp-home-screenshots/device-plugin-icons-20260831/{drawer,home}-rgb.png`) → **bars+version**: drawer row col 1 (`jjwxc`) top bar / `v1.4.2` / bottom bar, col 2/3 two bars empty; Home dock same (files OK, `weread`/`fanqie` empty bars, `jjwxc` `v1.4.2`). Crops: `/tmp/drawer-plugins-row.png`, `/tmp/home-dock-row.png`, `/tmp/drawer-tile5-icon-4x.png` (`v1.4.2`), `/tmp/home-dock3-icon-4x.png` (`v1.4.2`).

## Painters that can put version / two lines *into the icon slot*

Exhaustive search of every path that can touch the 62×64 icon rect (excluding the label under the tile):

* **`AppListActivity::drawItemIcon` (`firmware/src/activities/apps/AppListActivity.cpp:209, 260`)** — *the* drawer painter. `if (item.plugin && !pluginIcon.empty()) drawPluginIcon(renderer, pluginIcon, iconX, iconY) // 1=black` else `drawIcon(builtin 32×32)`. `pluginIcon` is `std::vector<uint8_t>` 512 B (62×64/8) filled by `HomeSceneAssetDecoder::decodeBmpFileTo1Bit(iconPath, buf, 62,64,8)` in `reload()`. Fallback on decode failure is `UIIcon::Library` (32×32 bookshelf, `LibraryIcon` — not two bars). No `version` / `versionCode` draw in this file at all.
* **`HomeSceneAssetDecoder::decodeAppIconForPublication` (`HomeSceneAssetDecoder.cpp:421`) + `HomeSceneAssetDecoder::decodeBmpFileTo1Bit` (`:227`)** — *the* Home dock painter. `resolveAppIconPath(installPath, iconField) → "/apps/<id>/icon_home.bmp"` → `decodeBmpFileTo1Bit(full, out, 62,64,8)` → `homeAddAssetToPublication(pub, {34,21,idx}, out)`. `decodeBuiltinFilesIconForPublication` (`:437`) is the only other Home icon path and is compiled (`kBuiltinFilesIcon` 512 B, inverted copy → `draw1BitAsset`). No `version` draw.
* **`GfxSceneRenderer` `kNodeIcon` (`firmware/src/ui/scene/GfxSceneRenderer.h` ~480)** — `if (assetBinding!=invalid) { a=assets.get(key); if(a&&valid) draw1BitAsset else drawRect(x,y,62,64,1)} else drawRect`. Asset missing → 1 px **rect border** (four edges), not two horizontals. No version.
* **Theme/scene app-icon nodes** (`themes/murphy-default/theme.json` + `compile_home_theme.py`) — the four dock slots are `repeat` `kBindingApps` children: `{"type":"icon","binding":"$item.icon","rect":[18,0,62,64]}` + `{"type":"text","binding":"$item.name","rect":[0,78,100,18]}`. No `$item.version` node; no bar nodes. No placeholder in `UiScenePackage` for version.
* **Any `version`/`versionCode` draw in icon context** — `grep -rn version firmware/src/activities/{apps,home}/` → ∅ for icon-slot paint. `grep -rn "draw.*version"` across `firmware/src` → only `OnlineOtaActivity`. No icon-slot version painter exists in the whitelist. `fillFallbackAppIcon` (`HomeSceneAssetDecoder.cpp:507`) is border+diagonal, never called for icons in current code.
* **`drawCoverPlaceholder` (`GfxSceneRenderer.h` round-4)** — cover-only (hero 110×180, recent 74×106), draws outer rounded rect + diagonal cross + book spine. Not used for `kNodeIcon`; not two horizontals.

Conclusion: **No whitelisted painter intentionally draws "two horizontals + version" into the 62×64 icon slot.** The bars+version must therefore be *the decoded bitmap data itself* (the 512 B content of `pluginIcon` / `pub.arena` for that slot), not an overlay. The file at `iconPath` on that device/SD, when decoded as 62×64 1-bpp with the current `isBlack = !bitSet` rule, **is** a bitmap of two full-width black rows (top at y≈16, bottom at y≈62) with `v1.4.2` (or `v1.2.3` on current device) rendered in the middle as 1-bit text. The packaged `plugins/m4-*-plugin/icon_home.bmp` files (574 B, `bfOffBits=62`, palette `[00 00 00 00, FF FF FF 00]`, ~15 % ink, correctly decoded as J/tomato/WeChat with `!bitSet`) are *not* that bitmap — the on-device file at `/apps/<id>/icon_home.bmp` that was actually read must be a different file.

## Why builtins look OK

* `builtin.files` (folder) at Home `slot 0` uses `decodeBuiltinFilesIconForPublication` — **compiled** `kBuiltinFilesIcon` (512 B, `~` inverted, `& 0xFC` padding), never touches SD or `decodeBmpFileTo1Bit`. AppList builtins use `builtinIconBitmap` → `FolderIcon` etc. (32×32) via `GfxRenderer::drawIcon`. Both bypass file decode, so the decode bug/file-content bug cannot affect them.

## Why only 晋江 shows version

The registry on that SD at capture time had three plugins with versions `1.7.2` / `1.3.3` / `1.2.3` (current) but the screenshot's version `v1.4.2` matches none of them — it matches the `NimBLE-Arduino` changelog version `1.4.2` present in `.pio/libdeps/…/CHANGELOG.md` in that worktree's build artifacts, **not** any `manifest.json`. The `v1.4.2` string appears in the 62×64 bitmap itself, so the file that was decoded for `com.jjwxc.client` on that SD was a BMP whose image data is the text `v1.4.2` between two rules, while the other two plugins' icon files decoded to two rules with empty middle (weaker). The only per-app distinguishing metadata that could select such a file is the `files` allowlist / `icon` field / `gbk_table.bin` presence: `jjwxc` declares `["gbk_table.bin","icon_home.bmp","main.xml"]` while `weread`/`fanqie` declare `["icon_home.bmp","main.xml"]`. A tooling or installer bug that mishandles the `gbk_table.bin` derivation (see `plugins/m4-jjwxc-plugin/tools/package.py` `_resolve_payload` which synthesizes `gbk_table.bin` from `firmware/lib/Txt/gbk_table.inc` when missing) could corrupt the zip central directory or cause the wrong entry to be extracted to `icon_home.bmp` on that SD. The installed file at `/apps/com.jjwxc.client/icon_home.bmp` could then be a different payload (e.g., a text or mis-converted file whose bitmap rendering happens to be `v1.4.2` between bars), while the other two remain as two-rule empty placeholders. This is consistent with the human's note that **NativeApp open works** (so `/apps/<id>/main.xml` is readable) — install partially succeeded but icon extraction was wrong. A direct filesystem check (`mcopy -i murphy-sd.img ::/apps/com.jjwxc.client/icon_home.bmp` hexdump + `bmpLen`/`bfOffBits`/`paletteLum` inspection) on the *device* SD that produced the screenshot would confirm.

An alternative device-specific hypothesis that also fits the two-bar geometry is a **BMP palette/offset mis-decode**: if the file's `bfOffBits` were `54` (header-only, no palette) instead of `62`, the decoder's `isBlack = !bitSet` reading of the 8-byte palette (`00 00 00 00 FF FF FF 00`) as the first two image rows would produce exactly one full-black row (`00…` → 62 black) and one mixed row, i.e., a top bar, with the remaining rows shifted and the bottom row also solid due to truncation/padding. The `v1.4.2` text would then be the label text that leaked into the image rows due to the 8-byte shift. This is the polarity/seek class the brief flags, and it is testable by inspecting `readU32LE(b+10)` on the on-device file.

## PluginIcon decode status on this lane (QEMU `485b7c7` + correct SD)

* `resolveAppIconPath("/apps/com.jjwxc.client","icon_home.bmp") → "/apps/com.jjwxc.client/icon_home.bmp"` — valid (`isValidInstallPath` + `isValidIconField` pass, no `..`/`//`/control, `<200` chars, constrained beneath base).
* `SdMan.openFileForRead("HAP", path, f)` → success (QEMU SD has LFN `icon_home.bmp`, not just `ICON_HO~1.BMP`; `mcopy` created LFN correctly).
* `Bitmap::parseHeaders()` → `Ok`, `getWidth()==62 && getHeight()==64`, `getRowBytes()==8`, matching `homePublicationSlotForKey({34,21,idx})`.
* `decodeBmpFileTo1Bit` loop for `bpp==1` → `f.read(rowBuf,8)` sequential from `bfOffBits=62`, `isBlack = !bitSet` (palette `[00,FF]` → `lum<128` threshold), correct placement `y = h-1-i` for bottom-up, `memcpy` to arena. Return `true` → `homeAddAssetToPublication` succeeds.
* `GfxSceneRenderer` finds `assets.get({34,21,idx})->valid()` → `draw1BitAsset` (1=black). **Proven**: QEMU drawer `J`/`tomato`/`WeChat` and dock `folder` are the expected assets, not bars.

Therefore on this lane the decode **succeeds with correct content**; fail would have produced `Library` (drawer, 32×32) or 1 px `drawRect` (Home), not bars. The on-device failure is an *incorrect successful decode* (wrong file content or wrong offset/palette), not a missing-file fallback.

## What to check next (one command each)

```bash
# 1. Dump the actual on-device icon files that produced the screenshot
for id in com.jjwxc.client com.fanqie.client com.weread.client; do
  mcopy -i /tmp/<device>_murphy-sd.img "::$id/icon_home.bmp" "/tmp/$id-icon.bmp" && \
  xxd "/tmp/$id-icon.bmp" | head -5 && \
  python3 -c "import struct; d=open('/tmp/$id-icon.bmp','rb').read(); print(struct.unpack('<I',d[10:14])[0], len(d), d[54:62].hex())"
done
# Expect bfOffBits=62, len=574, palette 00000000ffffff00. If bfOffBits=54 or len≠574 or palette inverted, that is the root.
# 2. Verify HAP tag: compare SdMan tag for icon vs cover — both use "HAP" in current code; confirm SDCardManager maps "HAP" to the user-data volume that contains /apps.
# 3. Palette-correct 1-bpp check: re-decode with paletteLum threshold instead of !bitSet and compare output to the bars image.
```

## Fix recommendation

Do **not** rewrite covers (`drawCoverPlaceholder` stays) and do not churn `theme.json`. If the on-device dump shows `bfOffBits=54` or an inverted palette (`FF…` first), the minimal correct fix inside the whitelist is a one-file change in `firmware/src/activities/home/HomeSceneAssetDecoder.cpp:349–359`:

* For `bpp==1`, do **not** use `isBlack = !bitSet` as a palette assumption. Use the `Bitmap` header's palette that `parseHeaders` already loaded (or re-derive `paletteLum` from the `bmpData` header as `decodeBmpBytesTo1Bit` does for `bpp<=8`): `idx = bitSet ? 1 : 0; isBlack = paletteLum[idx] < 128;`. This is already correct in `decodeBmpBytesTo1Bit` (`paletteLum` loop `:174`) but missing in the `decodeBmpFileTo1Bit` fast path. The fix is ~5 lines, keeps `HOME` and drawer on the same `paletteLum` contract, and does not hardcode snapshot probe sizes (64×64/50×80).

If the dump instead shows the file at that path is *not* a BMP at all (e.g., a text file containing `v1.4.2` or a mis-extracted `gbk_table.bin`), the fix is in the installer allowlist: ensure `plugins/m4-jjwxc-plugin/tools/package.py` `build_m4x` never lets the derived `gbk_table.bin` entry collide with `icon_home.bmp` in the zip, and that `M4xInstaller::extractEntryToFile` validates the entry name against `manifest.files` + `manifest.icon`/`entry` before writing to `iconPath`.

No `NativeAppActivity` / display-task handoff change (Luna). No `AppListActivity` mutex change. Leave `M4ProviderCoverCache.*` dirty untouched.

## Host commands + results (this lane)

```bash
git status --short --branch # agent/home-muse-impl …485b7c7 + dirty CoverCache
python3 -c "from PIL import Image; Image.open('plugins/m4-jjwxc-plugin/icon_home.bmp') …" # 574 B, palette [black,white], ~15 % ink if isBlack=!bitSet
# BMP header check (host)
python3 -c "import struct; d=open('plugins/m4-jjwxc-plugin/icon_home.bmp','rb').read(); print(struct.unpack('<I',d[10:14])[0])" # 62
# Decode host (paletteLum path)
python3 firmware/tools/compile_home_theme.py --theme themes/murphy-default/theme.json --out /tmp/x.m4theme --emit-header /tmp/x.h # 48728 B CRC32=7901d5bb
pytest firmware/tests/test_home_typography_polish.py firmware/tests/test_home_font_hierarchy.py firmware/tests/test_murphy_default_exact_geometry.py -q # 17 passed
# QEMU (own session, per-worktree)
PLATFORMIO_HOME_DIR=/tmp/pio_home2 pio run -e murphy_m4_qemu_plugin -j1 # SUCCESS 5352633 B
./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20 # READY after 2
m4adb screenshot /tmp/m4-home-new.pbm && python3 -c "from PIL import Image; Image.open('/tmp/m4-home-new.pbm').convert('L').save('/tmp/m4-home-new.png')"
m4adb tap 437 611 && m4adb screenshot /tmp/m4-drawer.pbm # AppList, correct line icons (see qemu-drawer-plugins-qemu.png)
```

## Deliverable

Report `docs/orchestration/rounds/round-5-muse-plugin-icon-rca.md` (this file). No code change in this RCA commit — fix is deferred pending on-device file dump. If the dump confirms `bfOffBits`/palette, the patch commit will be `round-5(muse): use paletteLum for 1-bpp icon decode`.

QEMU screenshots (local, this lane):

* Home (correct dock): `/tmp/m4-home-new.png` (also `/tmp/m4-round4-evidence/home.png`, 480×800, 10364 black)
* Drawer `AppList` (correct plugins): `/tmp/m4-drawer.png` (also `/tmp/m4-round4-evidence/drawer.png`)
* Plugin row crops (QEMU vs device): `/tmp/qemu-retest-drawer-plugins.png` (J/tomato/WeChat) vs `/tmp/drawer-plugins-row.png` / `/tmp/home-dock-row.png` (bars+v1.4.2)
* Per-icon 4× crops (device): `/tmp/drawer-tile5-icon-4x.png`, `/tmp/home-dock3-icon-4x.png`
* Source BMP renders (host, palette-correct): `/tmp/m4-jjwxc-plugin-decoded-4x.png` etc.
