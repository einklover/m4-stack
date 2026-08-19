#!/usr/bin/env bash
# Builds the M4 Screen Bridge debug APK with the local Android SDK tools only
# (no Gradle, no network). Run ./build.sh to build the APK and ./build.sh --test
# to run the off-device pure-Java self-checks.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP="$ROOT/app"
SRC="$APP/src/main/java/com/murphy/m4screenbridge"
TEST_SRC="$APP/src/test/java/com/murphy/m4screenbridge"
RES="$APP/src/main/res"
MANIFEST="$APP/src/main/AndroidManifest.xml"
OUT="$ROOT/build-out"
APK="$ROOT/m4-screen-bridge-debug.apk"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
[ -d "$SDK" ] || { echo "Android SDK not found (set ANDROID_HOME)"; exit 1; }

BT=""
for d in "$SDK"/build-tools/*/; do
  [ -x "$d/aapt2" ] && BT="$d"
done
[ -n "$BT" ] || { echo "no build-tools with aapt2 found"; exit 1; }
BT="${BT%/}"

PLATFORM=""
for d in "$SDK"/platforms/android-*/; do
  case "$d" in
    *android-[0-9][0-9]/) PLATFORM="$d" ;; # stable only, not previews like android-37.0
  esac
done
[ -n "$PLATFORM" ] || { echo "no stable android platform >= 30 found"; exit 1; }
PLATFORM="${PLATFORM%/}"
PLATFORM_JAR="$PLATFORM/android.jar"
COMPILE_SDK=$(basename "$PLATFORM" | sed 's/android-//')

command -v javac >/dev/null || { echo "javac not found"; exit 1; }

if [ "${1:-}" = "--test" ]; then
  echo "self-check (pure Java, no Android)"
  rm -rf "$OUT/testclasses"
  mkdir -p "$OUT/testclasses"
  PURE="Framebuffer Rle Header Preprocess PageStore XhsFeedParser"
  SRCS=""
  for c in $PURE; do SRCS="$SRCS $SRC/$c.java"; done
  PATCH_SRC="$SRC/browser/patch"
  STREAM_SRC="$SRC/browser/stream"
  javac --release 11 -d "$OUT/testclasses" \
    $SRCS \
    "$PATCH_SRC/LogicalMonoFrame.java" \
    "$PATCH_SRC/ExtraDimCompensation.java" \
    "$PATCH_SRC/RgbaFrameProbe.java" \
    "$PATCH_SRC/PatchRect.java" \
    "$PATCH_SRC/FramePatch.java" \
    "$PATCH_SRC/FrameDiffer.java" \
    "$PATCH_SRC/PatchApplier.java" \
    "$STREAM_SRC/M4B3.java" \
    "$STREAM_SRC/M4B3Exception.java" \
    "$STREAM_SRC/M4B3Outbound.java" \
    "$STREAM_SRC/M4B3Message.java" \
    "$STREAM_SRC/M4B3Codec.java" \
    "$STREAM_SRC/M4B3Sender.java" \
    "$STREAM_SRC/M4B3ReferenceReceiver.java" \
    "$STREAM_SRC/M4B3Framer.java" \
    "$STREAM_SRC/M4B3InputState.java" \
    "$STREAM_SRC/M4B3KeyState.java" \
    "$SRC/browser/discovery/M4LanDiscovery.java" \
    "$TEST_SRC/TestMain.java" \
    "$TEST_SRC/BrowserPatchTest.java" \
    "$TEST_SRC/M4B3ProtocolTest.java" \
    "$TEST_SRC/M4B3InputTest.java" \
    "$TEST_SRC/M4B3KeyTest.java" \
    "$TEST_SRC/M4LanDiscoveryTest.java"
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.TestMain
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.BrowserPatchTest
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.M4B3ProtocolTest
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.M4B3InputTest
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.M4B3KeyTest
  java -cp "$OUT/testclasses" com.murphy.m4screenbridge.M4LanDiscoveryTest
  exit 0
fi

rm -rf "$OUT"
mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/dex" "$OUT/apk"

echo "SDK=$SDK"
echo "build-tools=$BT"
echo "platform=$PLATFORM"

echo "aapt2 compile"
"$BT/aapt2" compile --dir "$RES" -o "$OUT/res"

echo "aapt2 link"
"$BT/aapt2" link \
  -o "$OUT/apk/unsigned.apk" \
  -I "$PLATFORM_JAR" \
  --manifest "$MANIFEST" \
  --min-sdk-version 30 \
  --target-sdk-version "$COMPILE_SDK" \
  --java "$OUT/gen" \
  "$OUT"/res/*.flat

echo "javac"
find "$SRC" -name '*.java' > "$OUT/sources.txt"
echo "$OUT/gen/com/murphy/m4screenbridge/R.java" >> "$OUT/sources.txt"
javac --release 11 -encoding UTF-8 \
  -classpath "$PLATFORM_JAR" \
  -d "$OUT/classes" \
  @"$OUT/sources.txt"

echo "d8"
find "$OUT/classes" -name '*.class' > "$OUT/classlist.txt"
"$BT/d8" --release --min-api 30 --lib "$PLATFORM_JAR" --output "$OUT/dex" @"$OUT/classlist.txt"

echo "package dex"
( cd "$OUT/dex" && zip -q -j "$OUT/apk/unsigned.apk" classes.dex )

echo "zipalign"
"$BT/zipalign" -f -p 4 "$OUT/apk/unsigned.apk" "$OUT/apk/aligned.apk"

echo "sign"
KEYSTORE="$ROOT/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" \
    -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
  --key-pass pass:android --out "$APK" "$OUT/apk/aligned.apk"

echo "verify"
"$BT/apksigner" verify --print-certs "$APK"

echo "built: $APK"
