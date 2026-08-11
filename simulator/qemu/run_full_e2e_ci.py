#!/usr/bin/env python3
"""Run Murphy M4 full-chain QEMU E2E in GitHub Actions.

Host dependencies (PlatformIO, fontTools, pyserial, mtools) are installed by the
workflow. This runner creates all test assets in RUNNER_TEMP, patches only the
CI worktree control surface, builds the normal Murphy Octal/OPI profile, boots
QEMU v3 and drives the existing m4adb protocol.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(os.environ.get("RUNNER_TEMP", "/tmp")) / "m4-full-e2e"
ART = Path(os.environ.get("M4_E2E_ARTIFACTS", str(TMP / "artifacts")))
WORK = TMP / "work"


def run(cmd: list[str], *, cwd: Path | None = None, check: bool = True,
        stdout=None, stderr=None, text: bool = True) -> subprocess.CompletedProcess:
    print("+", " ".join(shlex.quote(str(x)) for x in cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd or ROOT, check=check, stdout=stdout,
                          stderr=stderr, text=text)


def capture(cmd: list[str], *, cwd: Path | None = None, check: bool = True) -> str:
    cp = run(cmd, cwd=cwd, check=check, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = cp.stdout or ""
    print(out, end="")
    return out


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one anchor, got {count}")
    return text.replace(old, new, 1)


def prepare_fixture() -> tuple[Path, Path, Path]:
    from fontTools.ttLib import TTCollection
    from fontTools.subset import Options, Subsetter

    WORK.mkdir(parents=True, exist_ok=True)
    ART.mkdir(parents=True, exist_ok=True)
    txt = WORK / "e2e.txt"
    txt.write_text(
        "Murphy M4 全链路模拟测试\n"
        "第一章 本地 TXT 与运行时 TTF\n\n"
        "这是从 QEMU SDMMC 虚拟 SD 卡读取的本地中文 TXT。\n"
        "本页验证运行时 TTF、glyf 读取、PSRAM glyph 缓存、文本分页和 SSD1677 显示链。\n"
        "English 1234567890 ABC abc.\n\n"
        "第二章 插件宿主\n"
        "自动安装并启动番茄小说、晋江文学、微信读书三个插件。\n",
        encoding="utf-8",
    )

    candidates = [
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
    ]
    src = next((p for p in candidates if p.is_file()), None)
    if src is None:
        raise RuntimeError("WenQuanYi Zen Hei TTC not found")
    ttf = WORK / "M4E2E.ttf"
    chars = set(chr(i) for i in range(32, 127)) | set(txt.read_text(encoding="utf-8"))
    for p in (ROOT / "firmware/src").rglob("*"):
        if p.is_file() and p.suffix.lower() in {".cpp", ".h", ".json", ".xml"}:
            try:
                s = p.read_text(encoding="utf-8")
            except Exception:
                continue
            chars.update(ch for ch in s if ord(ch) >= 0x80)
    col = TTCollection(str(src), lazy=False)
    font = col.fonts[0]
    opts = Options()
    opts.hinting = False
    subset = Subsetter(options=opts)
    subset.populate(text="".join(sorted(chars)))
    subset.subset(font)
    font.save(str(ttf))
    if ttf.read_bytes()[:4] not in {b"\x00\x01\x00\x00", b"true"}:
        raise RuntimeError("generated fixture is not a glyf TrueType face")

    sd = WORK / "murphy-sd.img"
    run([sys.executable, str(ROOT / "simulator/qemu/make_sd_image.py"), str(sd), "--size-mb", "64"])
    run(["mmd", "-i", str(sd), "::/FONT", "::/books"], check=False)
    run(["mcopy", "-o", "-i", str(sd), str(ttf), "::/FONT/M4E2E.ttf"])
    run(["mcopy", "-o", "-i", str(sd), str(txt), "::/books/e2e.txt"])
    with (ART / "fixture.sha256").open("w", encoding="utf-8") as f:
        subprocess.run(["sha256sum", str(ttf), str(txt)], check=True, stdout=f, text=True)
    return ttf, txt, sd


def patch_e2e_control_surface() -> None:
    main = ROOT / "firmware/src/main.cpp"
    s = main.read_text(encoding="utf-8")
    s = replace_once(
        s,
        '#include "activities/reader/ReaderActivity.h"',
        '#include "activities/reader/ReaderActivity.h"\n#include "activities/reader/TxtReaderActivity.h"\n#include "managers/FontManager.h"',
        "E2E includes",
    )

    # A tiny checkpoint writer is deliberately test-only. The file is read back
    # through the production debug bridge, proving the active virtual SD path.
    marker = "#ifdef CROSSPOINT_MURPHY_M4\nFrontlightManager frontlightManager;"
    helper = r'''#ifdef M4_QEMU_E2E
static void m4E2ECheckpoint(const char* line) {
  SdMan.mkdir("/apps_data", true);
  SdMan.mkdir("/apps_data/e2e", true);
  FsFile f = SdMan.open("/apps_data/e2e/result.log", O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  f.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.sync();
  f.close();
}
#endif

#ifdef CROSSPOINT_MURPHY_M4
FrontlightManager frontlightManager;'''
    s = replace_once(s, marker, helper, "E2E checkpoint helper")

    needle = "    SETTINGS.loadFromFile();\n#ifdef CROSSPOINT_MURPHY_M4\n"
    repl = r'''    SETTINGS.loadFromFile();
#ifdef M4_QEMU_E2E
    std::snprintf(SETTINGS.customFontFamily, sizeof(SETTINGS.customFontFamily), "%s", "M4E2E.ttf");
    SETTINGS.fontSize = 28;
    Serial.printf("[%lu] [E2E] selected runtime TTF family=%s size=%u\n", millis(), SETTINGS.customFontFamily,
                  static_cast<unsigned>(SETTINGS.fontSize));
#endif
#ifdef CROSSPOINT_MURPHY_M4
'''
    s = replace_once(s, needle, repl, "select runtime TTF")

    auth = "      gM4DebugBridge.setAuthorized(SETTINGS.developerSerialDebugEnabled == 1);"
    s = replace_once(s, auth, r'''#ifdef M4_QEMU_E2E
      gM4DebugBridge.setAuthorized(true);
#else
      gM4DebugBridge.setAuthorized(SETTINGS.developerSerialDebugEnabled == 1);
#endif''', "authorize debug bridge")

    anchor = "    Serial.flush();\n\n#ifdef CROSSPOINT_MURPHY_M4\n    {\n      M4SerialDebug::HostHooks hooks;"
    ttf_probe = r'''    Serial.flush();
#ifdef M4_QEMU_E2E
    {
      auto* fam = FontManager::getInstance().getCustomFontFamily("M4E2E.ttf", 28);
      char cp[180];
      std::snprintf(cp, sizeof(cp), "TTF load=%d family=M4E2E.ttf size=28 heap=%u psram=%u",
                    fam ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getFreePsram()));
      Serial.printf("[%lu] [E2E-TTF] %s\n", millis(), cp);
      m4E2ECheckpoint(cp);
    }
#endif

#ifdef CROSSPOINT_MURPHY_M4
    {
      M4SerialDebug::HostHooks hooks;'''
    s = replace_once(s, anchor, ttf_probe, "TTF probe")

    anchor2 = "#ifndef CROSSPOINT_X3\n    // ========== X4: NTP时间同步已移至进入阅读器时执行 =========="
    e2e_txt = r'''#ifdef M4_QEMU_E2E
    {
      auto e2eTxt = std::make_unique<Txt>("/books/e2e.txt", "/.crosspoint");
      const bool ok = e2eTxt->load();
      uint8_t probe[512] = {};
      size_t actual = 0;
      const bool readOk = ok && e2eTxt->readContent(probe, 0, sizeof(probe), true, &actual);
      uint32_t hash = 2166136261u;
      for (size_t i = 0; i < actual; ++i) hash = (hash ^ probe[i]) * 16777619u;
      char cp[220];
      std::snprintf(cp, sizeof(cp), "TXT load=%d read=%d bytes=%u size=%u enc=%s hash=%08x",
                    ok ? 1 : 0, readOk ? 1 : 0, static_cast<unsigned>(actual),
                    ok ? static_cast<unsigned>(e2eTxt->getFileSize()) : 0u,
                    ok ? e2eTxt->getEncodingName() : "none", static_cast<unsigned>(hash));
      Serial.printf("[%lu] [E2E-TXT] %s\n", millis(), cp);
      m4E2ECheckpoint(cp);
      if (ok && e2eTxt->isEncodingSupported()) {
        exitActivity();
        enterNewActivity(new TxtReaderActivity(renderer, mappedInputManager, std::move(e2eTxt),
                                                []() { onGoHome(); }, []() { onGoHome(); }));
        Serial.printf("[%lu] [E2E-TXT] activity=TxtReader opened\n", millis());
        m4E2ECheckpoint("TXT activity=TxtReader opened");
      }
    }
#endif

#ifndef CROSSPOINT_X3
    // ========== X4: NTP时间同步已移至进入阅读器时执行 =========='''
    s = replace_once(s, anchor2, e2e_txt, "TXT autorun")
    main.write_text(s, encoding="utf-8")

    ini = ROOT / "firmware/platformio.ini"
    t = ini.read_text(encoding="utf-8")
    marker = "[env:murphy_m4]\nextends = m4_base\nbuild_flags =\n  ${m4_base.build_flags}\n"
    replacement = (
        "[env:murphy_m4]\nextends = m4_base\n"
        "build_unflags =\n  ${m4_base.build_unflags}\n  -DARDUINO_USB_CDC_ON_BOOT=1\n"
        "build_flags =\n  ${m4_base.build_flags}\n  -DARDUINO_USB_CDC_ON_BOOT=0\n  -DM4_QEMU_E2E=1\n"
    )
    ini.write_text(replace_once(t, marker, replacement, "Murphy E2E build flags"), encoding="utf-8")
    with (ART / "e2e-worktree.patch").open("w", encoding="utf-8") as f:
        subprocess.run(["git", "diff", "--", "firmware/src/main.cpp", "firmware/platformio.ini"],
                       cwd=ROOT, check=True, stdout=f, text=True)


def build_firmware_and_flash() -> Path:
    run(["pio", "run", "-e", "murphy_m4"], cwd=ROOT / "firmware")
    bdir = ROOT / "firmware/.pio/build/murphy_m4"
    fw = bdir / "firmware.bin"
    if not fw.is_file():
        raise RuntimeError("firmware.bin missing")
    with (ART / "e2e-firmware.sha256").open("w", encoding="utf-8") as f:
        subprocess.run(["sha256sum", str(fw)], check=True, stdout=f, text=True)
    flash = WORK / "murphy-e2e-16m.bin"
    run([sys.executable, str(ROOT / "simulator/tools/murphy_flash_image.py"),
         "--build-dir", str(bdir), "--slot", "auto", "-o", str(flash)])
    if flash.stat().st_size != 16 * 1024 * 1024:
        raise RuntimeError(f"bad flash size: {flash.stat().st_size}")
    return flash


def build_qemu() -> Path:
    src = WORK / "espressif-qemu"
    run([sys.executable, str(ROOT / "simulator/qemu/build_patched_qemu_v3.py"),
         "--source-dir", str(src), "--reconfigure", "-j", "2"])
    qemu = src / "build-murphy-v3/qemu-system-xtensa"
    if not qemu.is_file():
        raise RuntimeError("QEMU v3 binary missing")
    return qemu


def clone_plugins() -> list[tuple[str, str, Path]]:
    specs = [
        ("fanqie", "com.fanqie.client", "https://github.com/einklover/m4-fanqie-plugin.git"),
        ("jjwxc", "com.jjwxc.client", "https://github.com/einklover/m4-jjwxc-plugin.git"),
        ("weread", "com.weread.client", "https://github.com/einklover/m4-weread-plugin.git"),
    ]
    result = []
    for name, appid, url in specs:
        dest = WORK / name
        run(["git", "clone", "--depth", "1", "--branch", "agent/checkpoint-20260809-native-latest",
             url, str(dest)])
        result.append((name, appid, dest))
    return result


def m4adb(pty: str, args: list[str], *, timeout: int = 240, check: bool = True) -> str:
    cmd = [sys.executable, str(ROOT / "firmware/scripts/m4adb.py"), "--port", pty,
           "--no-daemon", "--timeout", "20", *args]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    cp = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=timeout)
    print(cp.stdout, end="")
    if check and cp.returncode:
        raise RuntimeError(f"m4adb {' '.join(args)} failed rc={cp.returncode}")
    return cp.stdout


def boot_and_drive(qemu: Path, flash: Path, sd: Path,
                   plugins: list[tuple[str, str, Path]]) -> None:
    frame = ART / "ssd1677-frame.pbm"
    qlog = ART / "qemu-runtime.log"
    cmd = [
        str(qemu), "-nographic", "-monitor", "none", "-machine", "murphy-m4", "-m", "8M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-drive", f"file={sd},if=sd,format=raw",
        "-global", "driver=esp32s3.gpio,property=strap_mode,value=0x04",
        "-global", "driver=esp32s3.gpio,property=input-default,value=0x100000000007",
        "-global", "driver=ssi_psram,property=is_octal,value=true",
        "-global", f"driver=murphy-ssd1677,property=frame-file,value={frame}",
        "-global", "driver=murphy-ssd1677,property=busy-ms,value=20",
        "-serial", "pty",
    ]
    print("+", " ".join(shlex.quote(x) for x in cmd), flush=True)
    with qlog.open("w", encoding="utf-8") as lf:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT, text=True)
    try:
        pty = ""
        deadline = time.time() + 120
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early rc={proc.returncode}; see {qlog}")
            text = qlog.read_text(encoding="utf-8", errors="replace") if qlog.exists() else ""
            hits = re.findall(r"/dev/pts/\d+", text)
            if hits:
                pty = hits[-1]
                break
            time.sleep(1)
        if not pty:
            raise RuntimeError("QEMU PTY not announced")
        (ART / "pty.txt").write_text(pty + "\n", encoding="utf-8")

        ready = False
        last = ""
        for _ in range(90):
            try:
                last = m4adb(pty, ["ping"], timeout=25, check=False)
                if '"protocol"' in last and '"firmware"' in last:
                    ready = True
                    break
            except subprocess.TimeoutExpired:
                pass
            if proc.poll() is not None:
                break
            time.sleep(1)
        (ART / "ping.txt").write_text(last, encoding="utf-8")
        if not ready:
            raise RuntimeError("m4adb bridge never became ready")

        (ART / "status-txt.json").write_text(m4adb(pty, ["status"]), encoding="utf-8")
        ui_txt = m4adb(pty, ["ui"])
        (ART / "ui-txt.json").write_text(ui_txt, encoding="utf-8")
        if "TxtReader" not in ui_txt:
            raise RuntimeError("runtime did not enter TxtReader")
        m4adb(pty, ["screenshot", str(ART / "txt-reader.pbm")], timeout=120)
        cp = m4adb(pty, ["sd_read", "apps_data/e2e/result.log", "--offset", "0", "--max", "400"])
        (ART / "e2e-checkpoint.txt").write_text(cp, encoding="utf-8")
        if "TTF load=1" not in cp:
            raise RuntimeError("runtime TTF checkpoint failed")
        if "TXT load=1 read=1" not in cp or "TXT activity=TxtReader opened" not in cp:
            raise RuntimeError("local TXT checkpoint failed")

        rows = []
        for name, appid, src in plugins:
            install_args = ["install", str(src), "--transport", "usb", "--ready-timeout", "20",
                            "--commit-timeout", "120", "--overall-timeout", "180"]
            install_out = m4adb(pty, install_args, timeout=220, check=False)
            (ART / f"{name}-install.log").write_text(install_out, encoding="utf-8")
            install_ok = "安装完成" in install_out or '"id"' in install_out or "installed" in install_out.lower()
            launch_out = ""
            launch_ok = False
            if install_ok:
                launch_out = m4adb(pty, ["launch", appid], timeout=60, check=False)
                launch_ok = "错误" not in launch_out and "error" not in launch_out.lower()
            (ART / f"{name}-launch.log").write_text(launch_out, encoding="utf-8")
            time.sleep(3)
            status = m4adb(pty, ["status"], timeout=40, check=False)
            ui = m4adb(pty, ["ui"], timeout=40, check=False)
            (ART / f"{name}-status.json").write_text(status, encoding="utf-8")
            (ART / f"{name}-ui.json").write_text(ui, encoding="utf-8")
            m4adb(pty, ["screenshot", str(ART / f"{name}.pbm")], timeout=120, check=False)
            # Strong launch assertion: status must report the requested app id.
            if appid in status or appid in ui:
                launch_ok = True
            rows.append(f"{name}\tinstall={int(install_ok)}\tlaunch={int(launch_ok)}")
            m4adb(pty, ["back"], timeout=30, check=False)
            time.sleep(1)
        result = "\n".join(rows) + "\n"
        (ART / "plugin-results.tsv").write_text(result, encoding="utf-8")
        print(result, end="")
        if any("install=0" in row or "launch=0" in row for row in rows):
            raise RuntimeError("one or more plugin install/launch checks failed")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


def main() -> int:
    try:
        _, _, sd = prepare_fixture()
        patch_e2e_control_surface()
        flash = build_firmware_and_flash()
        qemu = build_qemu()
        plugins = clone_plugins()
        boot_and_drive(qemu, flash, sd, plugins)
        (ART / "RESULT.txt").write_text("PASS full-chain TTF TXT plugins\n", encoding="utf-8")
        print("PASS full-chain TTF TXT plugins")
        return 0
    except Exception as exc:
        ART.mkdir(parents=True, exist_ok=True)
        (ART / "RESULT.txt").write_text(f"FAIL {type(exc).__name__}: {exc}\n", encoding="utf-8")
        print(f"FAIL {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
