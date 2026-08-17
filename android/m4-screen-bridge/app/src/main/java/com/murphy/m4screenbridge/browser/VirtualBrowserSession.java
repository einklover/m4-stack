package com.murphy.m4screenbridge.browser;

import android.content.Context;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;

import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchApplier;

import java.nio.ByteBuffer;
import java.util.Locale;

/**
 * App-owned 480x800 WebView VirtualDisplay session. In M1 it is owned by
 * BrowserBridgeService rather than MainActivity, and every RGBA frame is also fed through
 * the pure-Java dirty-tile patch core.
 */
public final class VirtualBrowserSession {
    public static final int WIDTH = 480;
    public static final int HEIGHT = 800;
    public static final int DENSITY_DPI = 160;
    private static final int MONO_THRESHOLD = 128;

    private final Context host;
    private final Handler main = new Handler(Looper.getMainLooper());

    private HandlerThread frameThread;
    private Handler frameHandler;
    private ImageReader reader;
    private VirtualDisplay virtualDisplay;
    private BrowserPresentation presentation;

    private volatile boolean active;
    private volatile long frameCount;
    private volatile int imageWidth;
    private volatile int imageHeight;
    private volatile int rowStride;
    private volatile int pixelStride;
    private volatile long lastFrameElapsedMs;
    private volatile long lastSignature;
    private volatile String currentUrl = "";
    private volatile String error = "";

    private byte[] logicalFrame;
    private long logicalFrameId = -1;
    private volatile long keyframeCount;
    private volatile long patchCount;
    private volatile long unchangedFrameCount;
    private volatile long patchPayloadBytes;
    private volatile int lastChangedTiles;
    private volatile int lastRectCount;
    private volatile int lastPatchBytes;
    private volatile double lastDirtyRatio;

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
        lastSignature = 0;
        logicalFrame = null;
        logicalFrameId = -1;
        keyframeCount = 0;
        patchCount = 0;
        unchangedFrameCount = 0;
        patchPayloadBytes = 0;
        lastChangedTiles = 0;
        lastRectCount = 0;
        lastPatchBytes = 0;
        lastDirtyRatio = 0;

        try {
            frameThread = new HandlerThread("m4-browser-frames");
            frameThread.start();
            frameHandler = new Handler(frameThread.getLooper());

            reader = ImageReader.newInstance(WIDTH, HEIGHT, PixelFormat.RGBA_8888, 3);
            reader.setOnImageAvailableListener(this::onImageAvailable, frameHandler);

            DisplayManager dm = (DisplayManager) host.getSystemService(Context.DISPLAY_SERVICE);
            if (dm == null) throw new IllegalStateException("DisplayManager unavailable");

            int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
                    | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY;
            virtualDisplay = dm.createVirtualDisplay(
                    "M4 EInk Browser",
                    WIDTH,
                    HEIGHT,
                    DENSITY_DPI,
                    reader.getSurface(),
                    flags);
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
        String html = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                + "<style>body{font-family:sans-serif;background:white;color:black;margin:24px}"
                + "#n{font-size:72px;font-weight:bold}</style>"
                + "<h2>M4 VirtualDisplay JavaScript self-test</h2>"
                + "<div id=n>0</div><p>This number changes once per second.</p>"
                + "<script>let n=0;setInterval(()=>document.getElementById('n').textContent=++n,1000);</script>";
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
                    .append(" | sig ").append(Long.toHexString(lastSignature));
            if (age >= 0) sb.append(" | age ").append(age).append("ms");
        }
        sb.append("\npatch frame ").append(logicalFrameId)
                .append(" | key ").append(keyframeCount)
                .append(" | delta ").append(patchCount)
                .append(" | same ").append(unchangedFrameCount)
                .append(" | last tiles ").append(lastChangedTiles).append('/').append(FrameDiffer.TOTAL_TILES)
                .append(" rects ").append(lastRectCount)
                .append(" bytes ").append(lastPatchBytes)
                .append(String.format(Locale.ROOT, " dirty %.2f%%", lastDirtyRatio * 100.0))
                .append(" | total bytes ").append(patchPayloadBytes);
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
            lastSignature = sampledSignature(pixels, imageWidth, imageHeight, rowStride, pixelStride);

            byte[] target = LogicalMonoFrame.fromRgba(pixels, imageWidth, imageHeight,
                    rowStride, pixelStride, MONO_THRESHOLD);
            processLogicalFrame(target);

            lastFrameElapsedMs = SystemClock.elapsedRealtime();
            frameCount++;
        } catch (Throwable t) {
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
        } finally {
            if (image != null) image.close();
        }
    }

    private void processLogicalFrame(byte[] target) {
        if (logicalFrame == null) {
            FramePatch patch = FrameDiffer.diff(null, -1, target, 0);
            logicalFrame = PatchApplier.apply(null, -1, patch);
            logicalFrameId = patch.frameId;
            keyframeCount++;
            recordPatch(patch);
            return;
        }

        FramePatch patch = FrameDiffer.diff(logicalFrame, logicalFrameId, target, logicalFrameId + 1);
        if (!patch.hasChanges()) {
            unchangedFrameCount++;
            lastChangedTiles = 0;
            lastRectCount = 0;
            lastPatchBytes = 0;
            lastDirtyRatio = 0;
            return;
        }
        logicalFrame = PatchApplier.apply(logicalFrame, logicalFrameId, patch);
        logicalFrameId = patch.frameId;
        if (patch.keyframe) keyframeCount++;
        else patchCount++;
        recordPatch(patch);
    }

    private void recordPatch(FramePatch patch) {
        lastChangedTiles = patch.changedTiles;
        lastRectCount = patch.rects.size();
        lastPatchBytes = patch.payloadBytes();
        lastDirtyRatio = patch.dirtyRatio();
        patchPayloadBytes += patch.payloadBytes();
    }

    /** Lightweight content signature retained for M0/M1 real-device compositor diagnostics. */
    static long sampledSignature(ByteBuffer source, int width, int height, int rowStride, int pixelStride) {
        ByteBuffer b = source.duplicate();
        long hash = 0xcbf29ce484222325L;
        int yStep = Math.max(1, height / 50);
        int xStep = Math.max(1, width / 30);
        for (int y = 0; y < height; y += yStep) {
            int row = y * rowStride;
            for (int x = 0; x < width; x += xStep) {
                int offset = row + x * pixelStride;
                if (offset < 0 || offset >= b.limit()) continue;
                int channels = Math.min(pixelStride, 4);
                for (int c = 0; c < channels && offset + c < b.limit(); c++) {
                    hash ^= b.get(offset + c) & 0xffL;
                    hash *= 0x100000001b3L;
                }
            }
        }
        return hash;
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
        logicalFrame = null;
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
