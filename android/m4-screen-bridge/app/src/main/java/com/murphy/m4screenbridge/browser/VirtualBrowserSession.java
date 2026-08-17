package com.murphy.m4screenbridge.browser;

import android.content.Context;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.hardware.display.VirtualDisplayConfig;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;

import android.util.Log;

import com.murphy.m4screenbridge.browser.patch.ExtraDimCompensation;
import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.RgbaFrameProbe;
import com.murphy.m4screenbridge.browser.stream.M4B3;
import com.murphy.m4screenbridge.browser.stream.M4B3ReferenceReceiver;
import com.murphy.m4screenbridge.browser.stream.M4B3Sender;

import java.nio.ByteBuffer;
import java.util.List;
import java.util.Locale;

/**
 * App-owned 480x800 WebView VirtualDisplay session. Owned by BrowserBridgeService.
 * ImageReader produces logical MONO1 frames; the M4B3 sender owns ACK/diff state
 * and never performs socket I/O on the capture callback.
 */
public final class VirtualBrowserSession {
    public static final int WIDTH = 480;
    public static final int HEIGHT = 800;
    public static final int DENSITY_DPI = 160;
    private static final int MONO_THRESHOLD = 128;
    private static final String TAG = "M4BrowserPatch";
    private static final int PIXEL_COUNT = LogicalMonoFrame.WIDTH * LogicalMonoFrame.HEIGHT;

    private final Context host;
    private final Handler main = new Handler(Looper.getMainLooper());

    private HandlerThread frameThread;
    private Handler frameHandler;
    private HandlerThread protocolThread;
    private Handler protocolHandler;
    private ImageReader reader;
    private VirtualDisplay virtualDisplay;
    private BrowserPresentation presentation;
    private M4B3Sender sender;
    private M4B3ReferenceReceiver localReceiver;

    private volatile boolean active;
    private volatile long frameCount;
    private volatile int imageWidth;
    private volatile int imageHeight;
    private volatile int rowStride;
    private volatile int pixelStride;
    private volatile long lastFrameElapsedMs;
    private volatile long lastRgbSignature;
    private volatile long lastRgbaSignature;
    private volatile int lastLumaMin;
    private volatile int lastLumaMax;
    private volatile int lastLumaMean;
    private volatile int lastRgbaDarkPixels;
    private volatile int lastBlackPixels;
    private volatile String lastProbePixels = "";
    private volatile int lastRgbGain256 = ExtraDimCompensation.UNITY_GAIN;
    private volatile long applyErrors;
    private volatile String currentUrl = "";
    private volatile String error = "";

    private volatile long logicalFrameId = -1;
    private volatile long keyframeCount;
    private volatile long patchCount;
    private volatile long unchangedFrameCount;
    private volatile long patchPayloadBytes;
    private volatile int lastChangedTiles;
    private volatile int lastRectCount;
    private volatile int lastPatchBytes;
    private volatile double lastDirtyRatio;
    private volatile boolean protocolInFlight;
    private volatile boolean protocolPending;
    private volatile long nackRecoveries;
    private volatile int lastAckedCrc;

    public VirtualBrowserSession(Context host) {
        if (host == null) throw new IllegalArgumentException("host is null");
        this.host = host.getApplicationContext();
    }

    public void start(String rawUrl) {
        ensureMainThread();
        stop();
        String url = normalizeUrl(rawUrl);
        currentUrl = url;
        error = "";
        frameCount = 0;
        imageWidth = 0;
        imageHeight = 0;
        rowStride = 0;
        pixelStride = 0;
        lastFrameElapsedMs = 0;
        lastRgbSignature = 0;
        lastRgbaSignature = 0;
        lastLumaMin = 0;
        lastLumaMax = 0;
        lastLumaMean = 0;
        lastRgbaDarkPixels = 0;
        lastBlackPixels = 0;
        lastProbePixels = "";
        lastRgbGain256 = ExtraDimCompensation.UNITY_GAIN;
        applyErrors = 0;
        logicalFrameId = -1;
        keyframeCount = 0;
        patchCount = 0;
        unchangedFrameCount = 0;
        patchPayloadBytes = 0;
        lastChangedTiles = 0;
        lastRectCount = 0;
        lastPatchBytes = 0;
        lastDirtyRatio = 0;
        protocolInFlight = false;
        protocolPending = false;
        nackRecoveries = 0;
        lastAckedCrc = 0;

        try {
            protocolThread = new HandlerThread("m4-browser-m4b3");
            protocolThread.start();
            protocolHandler = new Handler(protocolThread.getLooper());
            localReceiver = new M4B3ReferenceReceiver();
            sender = new M4B3Sender(packet -> {
                Handler h = protocolHandler;
                if (h != null) h.post(() -> deliverLoopback(packet));
            });
            sender.connect();

            frameThread = new HandlerThread("m4-browser-frames");
            frameThread.start();
            frameHandler = new Handler(frameThread.getLooper());

            reader = ImageReader.newInstance(WIDTH, HEIGHT, PixelFormat.RGBA_8888, 3);
            reader.setOnImageAvailableListener(this::onImageAvailable, frameHandler);

            DisplayManager dm = (DisplayManager) host.getSystemService(Context.DISPLAY_SERVICE);
            if (dm == null) throw new IllegalStateException("DisplayManager unavailable");

            int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY;
            // Motorola Android 16 composed this display at brightnessDefault=0.0
            // (minimum), so CSS #ffffff arrived as rgba(38,37,35,255) and never
            // crossed the MONO1 threshold. Force full brightness on API 36+.
            if (Build.VERSION.SDK_INT >= 36) {
                VirtualDisplayConfig config = new VirtualDisplayConfig.Builder(
                        "M4 EInk Browser", WIDTH, HEIGHT, DENSITY_DPI)
                        .setSurface(reader.getSurface())
                        .setFlags(flags)
                        .setDefaultBrightness(1.0f)
                        .build();
                virtualDisplay = dm.createVirtualDisplay(config);
            } else {
                virtualDisplay = dm.createVirtualDisplay(
                        "M4 EInk Browser",
                        WIDTH,
                        HEIGHT,
                        DENSITY_DPI,
                        reader.getSurface(),
                        flags);
            }
            if (virtualDisplay == null) throw new IllegalStateException("createVirtualDisplay returned null");

            Display display = virtualDisplay.getDisplay();
            if (display == null) throw new IllegalStateException("virtual display has no Display");

            presentation = new BrowserPresentation(host, display, url);
            presentation.show();
            active = true;
        } catch (Throwable t) {
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
            stopInternal(false);
        }
    }

    public void startJavaScriptSelfTest() {
        // 480x320 block at y=160 is exactly 40% of 480x800 / 600 of 1500 tiles,
        // so a black<->white flip stays under the 60% keyframe threshold.
        String html = "<!doctype html><html><head>"
                + "<meta name=viewport content='width=480,initial-scale=1,user-scalable=no'>"
                + "<style>"
                + "html,body{margin:0;padding:0;background:#ffffff;color:#000000;"
                + "width:480px;height:800px;overflow:hidden;font-family:sans-serif}"
                + "#hud{position:absolute;left:0;top:0;width:480px;height:48px;"
                + "font-size:20px;font-weight:bold;padding:8px 12px;box-sizing:border-box;z-index:2}"
                + "#blk{position:absolute;left:0;top:160px;width:480px;height:320px;"
                + "background:#000000;z-index:1}"
                + "</style></head><body>"
                + "<div id=hud>M4 dirty-source <span id=n>0</span></div>"
                + "<div id=blk></div>"
                + "<script>let n=0,on=true;setInterval(function(){"
                + "n++;on=!on;"
                + "document.getElementById('n').textContent=n;"
                + "document.getElementById('blk').style.background=on?'#000000':'#ffffff';"
                + "},1000);</script></body></html>";
        start("data:text/html;charset=utf-8," + Uri.encode(html));
    }

    public void stop() {
        ensureMainThread();
        stopInternal(true);
    }

    public boolean isActive() {
        return active;
    }

    public String snapshot() {
        if (!active) {
            return error.isEmpty() ? "虚拟浏览器 M1：未启动" : "虚拟浏览器 M1：启动失败 | " + error;
        }
        long age = lastFrameElapsedMs == 0 ? -1
                : Math.max(0, SystemClock.elapsedRealtime() - lastFrameElapsedMs);
        StringBuilder sb = new StringBuilder();
        sb.append("虚拟浏览器 M1：运行中 | display ")
                .append(WIDTH).append('x').append(HEIGHT)
                .append('@').append(DENSITY_DPI).append("dpi")
                .append(" | frames ").append(frameCount);
        if (imageWidth > 0) {
            sb.append(" | image ").append(imageWidth).append('x').append(imageHeight)
                    .append(" stride ").append(rowStride).append('/').append(pixelStride)
                    .append(" | rgbSig ").append(Long.toHexString(lastRgbSignature))
                    .append(" | rgbaSig ").append(Long.toHexString(lastRgbaSignature));
            if (age >= 0) sb.append(" | age ").append(age).append("ms");
            sb.append("\nluma min/max/mean ")
                    .append(lastLumaMin).append('/').append(lastLumaMax).append('/').append(lastLumaMean)
                    .append(" | rgbaDark ").append(lastRgbaDarkPixels)
                    .append(" | black ").append(lastBlackPixels).append('/').append(PIXEL_COUNT)
                    .append(String.format(Locale.ROOT, " (%.2f%%)",
                            lastBlackPixels * 100.0 / PIXEL_COUNT));
            if (!lastProbePixels.isEmpty()) sb.append("\npx ").append(lastProbePixels);
            sb.append(" | gain ").append(lastRgbGain256);
        }
        sb.append("\npatch frame ").append(logicalFrameId)
                .append(" | key ").append(keyframeCount)
                .append(" | delta ").append(patchCount)
                .append(" | same ").append(unchangedFrameCount)
                .append(" | last tiles ").append(lastChangedTiles).append('/').append(FrameDiffer.TOTAL_TILES)
                .append(" rects ").append(lastRectCount)
                .append(" bytes ").append(lastPatchBytes)
                .append(String.format(Locale.ROOT, " dirty %.2f%%", lastDirtyRatio * 100.0))
                .append(" | total bytes ").append(patchPayloadBytes)
                .append(" | applyErr ").append(applyErrors)
                .append("\nM4B3 loopback ")
                .append(protocolInFlight ? "in-flight" : "idle")
                .append(protocolPending ? " pending" : "")
                .append(" nack ").append(nackRecoveries)
                .append(" crc ").append(M4B3.crcHex(lastAckedCrc));
        if (!currentUrl.isEmpty()) sb.append("\nURL: ").append(currentUrl);
        if (!error.isEmpty()) sb.append("\n错误: ").append(error);
        return sb.toString();
    }

    private void onImageAvailable(ImageReader imageReader) {
        Image image = null;
        try {
            image = imageReader.acquireLatestImage();
            if (image == null) return;
            Image.Plane[] planes = image.getPlanes();
            if (planes == null || planes.length == 0) return;
            Image.Plane plane = planes[0];

            imageWidth = image.getWidth();
            imageHeight = image.getHeight();
            rowStride = plane.getRowStride();
            pixelStride = plane.getPixelStride();
            ByteBuffer pixels = plane.getBuffer();
            RgbaFrameProbe probe = RgbaFrameProbe.inspect(pixels, imageWidth, imageHeight,
                    rowStride, pixelStride);
            lastRgbSignature = probe.rgbSignature;
            lastRgbaSignature = probe.rgbaSignature;
            lastLumaMin = probe.lumaMin;
            lastLumaMax = probe.lumaMax;
            lastLumaMean = probe.meanLuma();
            lastRgbaDarkPixels = probe.lumaBelowThreshold;
            lastProbePixels = RgbaFrameProbe.formatPixel(10, 10,
                    RgbaFrameProbe.pixelRgba(pixels, 10, 10, rowStride, pixelStride))
                    + " "
                    + RgbaFrameProbe.formatPixel(240, 320,
                    RgbaFrameProbe.pixelRgba(pixels, 240, 320, rowStride, pixelStride))
                    + " "
                    + RgbaFrameProbe.formatPixel(240, 700,
                    RgbaFrameProbe.pixelRgba(pixels, 240, 700, rowStride, pixelStride));

            lastRgbGain256 = ExtraDimCompensation.autoGain256(lastLumaMax, MONO_THRESHOLD);
            byte[] target = LogicalMonoFrame.fromRgba(pixels, imageWidth, imageHeight,
                    rowStride, pixelStride, MONO_THRESHOLD, lastRgbGain256);
            lastBlackPixels = LogicalMonoFrame.countBlack(target);
            try {
                processLogicalFrame(target);
            } catch (Throwable t) {
                applyErrors++;
                throw t;
            }

            lastFrameElapsedMs = SystemClock.elapsedRealtime();
            frameCount++;
            logFrame();
        } catch (Throwable t) {
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
            Log.e(TAG, "frame failed: " + error, t);
        } finally {
            if (image != null) image.close();
        }
    }

    private void processLogicalFrame(byte[] target) {
        M4B3Sender s = sender;
        if (s == null) throw new IllegalStateException("M4B3 sender not started");
        s.offerFrame(target);
        refreshProtocolStats();
    }

    private void deliverLoopback(byte[] packet) {
        M4B3Sender s = sender;
        M4B3ReferenceReceiver r = localReceiver;
        if (s == null || r == null) return;
        try {
            List<byte[]> replies = r.handle(packet);
            for (byte[] reply : replies) s.receive(reply);
            refreshProtocolStats();
        } catch (Throwable t) {
            applyErrors++;
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
            Log.e(TAG, "M4B3 loopback failed: " + error, t);
        }
    }

    private void refreshProtocolStats() {
        M4B3Sender s = sender;
        if (s == null) return;
        M4B3Sender.Stats st = s.stats();
        logicalFrameId = st.inFlight ? st.inFlightFrameId : st.ackedFrameId;
        keyframeCount = st.keyframesSent;
        patchCount = st.patchesSent;
        unchangedFrameCount = st.unchangedSuppressed;
        lastChangedTiles = st.lastChangedTiles;
        lastRectCount = st.lastRectCount;
        lastPatchBytes = st.lastPatchBytes;
        lastDirtyRatio = st.lastDirtyRatio;
        protocolInFlight = st.inFlight;
        protocolPending = st.hasPending;
        nackRecoveries = st.nackRecoveries;
        lastAckedCrc = st.ackedCrc;
        patchPayloadBytes = st.payloadBytesSent;
    }

    private void logFrame() {
        long age = lastFrameElapsedMs == 0 ? -1
                : Math.max(0, SystemClock.elapsedRealtime() - lastFrameElapsedMs);
        Log.i(TAG, String.format(Locale.ROOT,
                "frames=%d patch=%d key=%d delta=%d same=%d tiles=%d/%d dirty=%.2f%% "
                        + "rgbSig=%x rgbaSig=%x luma=%d/%d/%d rgbaDark=%d black=%d applyErr=%d "
                        + "stride=%d/%d gain=%d age=%dms %s",
                frameCount, logicalFrameId, keyframeCount, patchCount, unchangedFrameCount,
                lastChangedTiles, FrameDiffer.TOTAL_TILES, lastDirtyRatio * 100.0,
                lastRgbSignature, lastRgbaSignature, lastLumaMin, lastLumaMax, lastLumaMean,
                lastRgbaDarkPixels, lastBlackPixels, applyErrors, rowStride, pixelStride,
                lastRgbGain256, age, lastProbePixels));
    }

    private void stopInternal(boolean clearError) {
        active = false;
        if (presentation != null) {
            try {
                presentation.destroyBrowser();
                presentation.dismiss();
            } catch (Throwable ignored) {
            }
            presentation = null;
        }
        if (virtualDisplay != null) {
            try {
                virtualDisplay.release();
            } catch (Throwable ignored) {
            }
            virtualDisplay = null;
        }
        if (reader != null) {
            try {
                reader.setOnImageAvailableListener(null, null);
                reader.close();
            } catch (Throwable ignored) {
            }
            reader = null;
        }
        if (frameThread != null) {
            frameThread.quitSafely();
            frameThread = null;
            frameHandler = null;
        }
        if (protocolThread != null) {
            protocolThread.quitSafely();
            protocolThread = null;
            protocolHandler = null;
        }
        if (sender != null) {
            sender.disconnect();
            sender = null;
        }
        localReceiver = null;
        logicalFrameId = -1;
        if (clearError) error = "";
    }

    private static String normalizeUrl(String raw) {
        String url = raw == null ? "" : raw.trim();
        if (url.isEmpty()) return "https://example.com/";
        String lower = url.toLowerCase(Locale.ROOT);
        if (lower.startsWith("http://") || lower.startsWith("https://")
                || lower.startsWith("data:") || lower.startsWith("about:")) {
            return url;
        }
        return "https://" + url;
    }

    private void ensureMainThread() {
        if (Looper.myLooper() != main.getLooper()) {
            throw new IllegalStateException("VirtualBrowserSession must be controlled from main thread");
        }
    }

    private static String safeMessage(Throwable t) {
        String msg = t.getMessage();
        return msg == null ? "" : msg;
    }
}
