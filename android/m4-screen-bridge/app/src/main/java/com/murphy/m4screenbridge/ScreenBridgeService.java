package com.murphy.m4screenbridge;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.hardware.HardwareBuffer;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;

/** Accessibility service: captures the book app screen, converts it to M4 framebuffer
 * pages, serves them over TCP 48624, and prefetches with reader page-turn taps. */
public class ScreenBridgeService extends AccessibilityService {
    private static final String TAG = "M4ScreenBridge";
    public static final String PREFS = "m4screenbridge";
    private static final int PRE_AHEAD = 2;

    public static volatile ScreenBridgeService instance;
    public static volatile Runnable statusListener;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object loopLock = new Object();

    private volatile boolean enabled = false;
    private volatile boolean sessionActive = false;
    private volatile int sessionGeneration = 0;
    private volatile String captureError = "";
    private volatile int captureErrorCode = 0;
    private volatile boolean liveFallback = false;
    private volatile String foregroundPackage = "";
    private volatile String foregroundClass = "";
    private volatile String readerPackage = "";
    private volatile String readerClass = "";
    private ExecutorService captureExecutor;
    private Thread prefetchThread;
    private HttpServer server;
    private BridgeContentApi contentApi;
    private PageStore store;
    private int displayWidth = 1080;
    private int displayHeight = 2400;

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
        WindowManager wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        if (wm != null) {
            android.graphics.Rect bounds = wm.getCurrentWindowMetrics().getBounds();
            displayWidth = bounds.width();
            displayHeight = bounds.height();
        }
        store = new PageStore();
        captureExecutor = java.util.concurrent.Executors.newSingleThreadExecutor();
        contentApi = new BridgeContentApi(this, main);
        server = new HttpServer(store, this::wakeLoop, this::tapFromM4, this::cacheActive,
                contentApi);
        server.start();
        enabled = true;
        prefetchThread = new Thread(this::captureLoop, "m4-prefetch");
        prefetchThread.setDaemon(true);
        prefetchThread.start();
        notifyStatus();
    }

    @Override
    public void onDestroy() {
        sessionActive = false;
        sessionGeneration++;
        enabled = false;
        if (server != null) server.stop();
        if (contentApi != null) contentApi.shutdown();
        if (prefetchThread != null) prefetchThread.interrupt();
        if (captureExecutor != null) captureExecutor.shutdownNow();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        if (event == null) return;
        CharSequence pkg = event.getPackageName();
        CharSequence cls = event.getClassName();
        if (pkg != null) foregroundPackage = pkg.toString();
        // Content-change events report RecyclerView/TextView class names. Only
        // window-state events identify the actual Activity/window we can use
        // for navigation diagnostics.
        if (cls != null && event.getEventType() == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED) {
            foregroundClass = cls.toString();
        }
    }

    @Override
    public void onInterrupt() {
    }

    public static boolean startSession(long delayMs) {
        ScreenBridgeService s = instance;
        if (s == null || !s.enabled) return false;
        s.beginSession(delayMs);
        return true;
    }

    public static void stopSession() {
        ScreenBridgeService s = instance;
        if (s != null) s.endSession();
    }

    public static boolean isSessionActive() {
        ScreenBridgeService s = instance;
        return s != null && s.enabled && s.sessionActive;
    }

    public static int captureErrorCodeSnapshot() {
        ScreenBridgeService s = instance;
        return s == null ? -9 : s.captureErrorCode;
    }

    public static void preferencesChanged() {
        ScreenBridgeService s = instance;
        if (s != null && s.store != null) {
            s.liveFallback = false;
            s.captureError = "";
            s.captureErrorCode = 0;
            s.store.reset();
            s.wakeLoop();
            s.notifyStatus();
        }
    }

    String foregroundPackageSnapshot() { return foregroundPackage; }

    String foregroundClassSnapshot() { return foregroundClass; }

    boolean globalBack() { return performGlobalAction(GLOBAL_ACTION_BACK); }

    boolean swipeScreen(float startX, float startY, float endX, float endY, long durationMs) {
        Path p = new Path();
        p.moveTo(startX, startY);
        p.lineTo(endX, endY);
        GestureDescription g = new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(p, 0,
                        Math.max(120, durationMs)))
                .build();
        return dispatchGesture(g, null, null);
    }

    Bitmap captureScreenBitmap() {
        final CountDownLatch done = new CountDownLatch(1);
        final Bitmap[] result = new Bitmap[1];
        main.post(() -> takeScreenshot(0, captureExecutor, new TakeScreenshotCallback() {
            @Override public void onSuccess(ScreenshotResult r) {
                HardwareBuffer hb = r.getHardwareBuffer();
                Bitmap wrapped = null;
                try {
                    wrapped = Bitmap.wrapHardwareBuffer(hb, r.getColorSpace());
                    if (wrapped != null) result[0] = wrapped.copy(Bitmap.Config.ARGB_8888, false);
                } finally {
                    if (wrapped != null) wrapped.recycle();
                    hb.close();
                    done.countDown();
                }
            }

            @Override public void onFailure(int errorCode) { done.countDown(); }
        }));
        try {
            if (!done.await(4, TimeUnit.SECONDS)) return null;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return null;
        }
        return result[0];
    }

    private void beginSession(long delayMs) {
        sessionActive = false;
        int generation = ++sessionGeneration;
        captureError = "";
        captureErrorCode = 0;
        liveFallback = false;
        readerPackage = "";
        readerClass = "";
        store.reset();
        notifyStatus();
        main.postDelayed(() -> {
            if (enabled && generation == sessionGeneration) {
                sessionActive = true;
                wakeLoop();
                notifyStatus();
            }
        }, Math.max(0, delayMs));
    }

    private void endSession() {
        sessionActive = false;
        sessionGeneration++;
        liveFallback = false;
        readerPackage = "";
        readerClass = "";
        store.reset();
        wakeLoop();
        notifyStatus();
    }

    public static String snapshot() {
        ScreenBridgeService s = instance;
        if (s == null || s.server == null || !s.enabled) return "服务未运行";
        if (s.server.startError() != null) return "端口启动失败：" + s.server.startError();
        return (s.sessionActive ? "会话进行中" : "会话已停止")
                + " | TCP " + HttpServer.PORT + " | 页面 " + s.store.lo() + ".." + s.store.hi()
                + "（" + s.store.count() + " 页）| M4 已读 " + s.store.consumedIndex()
                + " | " + (s.cacheActive() ? "缓存模式" : "实时模式")
                + " | " + (s.server.connected() ? "M4 已连接" : "等待 M4 连接")
                + (s.captureError.isEmpty() ? "" : "\n" + s.captureError);
    }

    private void notifyStatus() {
        Runnable l = statusListener;
        if (l != null) l.run();
    }

    private void wakeLoop() {
        synchronized (loopLock) {
            loopLock.notifyAll();
        }
    }

    private void sleepLoop(long ms) {
        synchronized (loopLock) {
            try {
                loopLock.wait(ms);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private void captureLoop() {
        boolean awaitingReader = false;
        int observedGeneration = -1;
        while (enabled) {
            try {
                if (!sessionActive) {
                    awaitingReader = false;
                    sleepLoop(500);
                    continue;
                }
                if (observedGeneration != sessionGeneration) {
                    observedGeneration = sessionGeneration;
                    awaitingReader = false;
                }
                int hi = store.hi();
                if (!cacheActive()) {
                    // A user-disabled cache refreshes after each forwarded touch.
                    // An automatic fallback is also sampled so a timed ad can
                    // return to cached reading without requiring an extra tap.
                    if (hi == -1 || liveFallback) capturePage(0);
                    sleepLoop(liveFallback ? 1200 : 400);
                    continue;
                }
                int consumed = store.consumedIndex();
                if (hi == -1) {
                    capturePage(0);
                    sleepLoop(1000);
                    continue;
                }
                int target = Math.max(hi, consumed + PRE_AHEAD);
                if (hi < target) {
                    if (!awaitingReader) {
                        if (!turnForward()) {
                            sleepLoop(600);
                            continue;
                        }
                        sleepLoop(prefs().prefetchMs);
                    }
                    awaitingReader = !capturePage(hi + 1);
                    if (awaitingReader) sleepLoop(700);
                } else {
                    sleepLoop(400);
                }
            } catch (Exception e) {
                sleepLoop(1000);
            }
        }
    }

    private boolean capturePage(final int page) {
        final int generation = sessionGeneration;
        byte[] frame = captureFrame(generation);
        if (frame != null && sessionActive && generation == sessionGeneration) {
            store.put(cacheActive() ? page : 0, frame);
            notifyStatus();
            return true;
        }
        return false;
    }

    private byte[] captureFrame(final int generation) {
        for (int attempt = 0; attempt < 3 && enabled && sessionActive; attempt++) {
            final CountDownLatch done = new CountDownLatch(1);
            final byte[][] res = new byte[1][];
            main.post(() -> {
                try {
                    takeScreenshot(0, captureExecutor, new TakeScreenshotCallback() {
                        @Override
                        public void onSuccess(ScreenshotResult r) {
                            HardwareBuffer hb = r.getHardwareBuffer();
                            Bitmap wrapped = null;
                            Bitmap bmp = null;
                            try {
                                wrapped = Bitmap.wrapHardwareBuffer(hb, r.getColorSpace());
                                if (wrapped != null) {
                                    bmp = wrapped.copy(Bitmap.Config.ARGB_8888, false);
                                    if (bmp == null) throw new IllegalStateException("bitmap copy failed");
                                    res[0] = buildFramebuffer(bmp);
                                    if (res[0] != null && captureErrorCode != -4) {
                                        captureError = "";
                                        captureErrorCode = 0;
                                    }
                                }
                            } catch (Throwable t) {
                                Log.e(TAG, "Screenshot processing failed", t);
                                res[0] = null;
                                captureErrorCode = -3;
                                captureError = "处理截图失败：" + t.getClass().getSimpleName();
                            } finally {
                                if (bmp != null) bmp.recycle();
                                if (wrapped != null) wrapped.recycle();
                                hb.close();
                                done.countDown();
                            }
                        }

                        @Override
                        public void onFailure(int errorCode) {
                            captureErrorCode = errorCode;
                            captureError = "截图失败（系统错误 " + errorCode + "）";
                            done.countDown();
                        }
                    });
                } catch (SecurityException e) {
                    captureErrorCode = -1;
                    captureError = "无障碍服务缺少截图能力，请关闭后重新开启该服务";
                    done.countDown();
                    notifyStatus();
                } catch (Throwable t) {
                    captureErrorCode = -2;
                    captureError = "截图调用失败：" + t.getClass().getSimpleName();
                    done.countDown();
                    notifyStatus();
                }
            });
            try {
                if (!done.await(4, TimeUnit.SECONDS)) continue;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
            if (res[0] != null && sessionActive && generation == sessionGeneration) {
                return res[0];
            }
            sleepLoop(300);
        }
        return null;
    }

    private boolean turnForward() {
        return tapScreen(displayWidth * 0.85f, displayHeight * 0.50f);
    }

    boolean tapScreen(float x, float y) {
        final CountDownLatch done = new CountDownLatch(1);
        final boolean[] ok = new boolean[1];
        Path p = new Path();
        p.moveTo(x, y);
        final GestureDescription g = new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(p, 0, 80))
                .build();
        if (Looper.myLooper() == main.getLooper()) {
            return dispatchGesture(g, null, null);
        }
        main.post(() -> dispatchGesture(g, new GestureResultCallback() {
            @Override
            public void onCompleted(GestureDescription gd) {
                ok[0] = true;
                done.countDown();
            }

            @Override
            public void onCancelled(GestureDescription gd) {
                done.countDown();
            }
        }, null));
        try {
            if (!done.await(2, TimeUnit.SECONDS)) return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return ok[0];
    }

    private boolean tapFromM4(int x, int y) {
        if (!sessionActive || cacheActive()) return false;
        float shownW = (float) Preprocess.H * displayWidth / displayHeight;
        float left = (Preprocess.W - shownW) / 2f;
        float phoneX = (x - left) * displayWidth / shownW;
        float phoneY = (float) y * displayHeight / Preprocess.H;
        phoneX = Math.max(0, Math.min(displayWidth - 1, phoneX));
        phoneY = Math.max(0, Math.min(displayHeight - 1, phoneY));
        if (!tapScreen(phoneX, phoneY)) return false;
        try {
            Thread.sleep(prefs().prefetchMs);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return capturePage(0);
    }

    private byte[] buildFramebuffer(Bitmap src) {
        Prefs p = prefs();
        int[] gray;
        if (!p.cacheEnabled) {
            gray = toGray(src, "fit");
        } else {
            int sourceH = Math.max(1, Math.round(src.getHeight()
                    * (float) Preprocess.W / src.getWidth()));
            int[] sourceGray = toGrayAtWidth(src, sourceH);
            boolean textPage = Preprocess.looksLikeTextPage(sourceGray, sourceH, p.threshold);
            String activePackage = foregroundPackage;
            String activeClass = foregroundClass;
            if (readerPackage.isEmpty() && textPage && eligibleReaderPackage(activePackage)) {
                readerPackage = activePackage;
                readerClass = activeClass;
            }
            boolean sameReader = !readerPackage.isEmpty()
                    && readerPackage.equals(activePackage)
                    && (readerClass.isEmpty() || activeClass.isEmpty()
                        || readerClass.equals(activeClass));
            if (!textPage || !sameReader) {
                setLiveFallback(true);
                captureErrorCode = -4;
                captureError = "检测到广告、弹窗或非正文，已自动切换实时触摸模式";
                notifyStatus();
                gray = toGray(src, "fit");
            } else {
                setLiveFallback(false);
                captureErrorCode = 0;
                captureError = "";
                gray = "cover".equals(p.cropMode)
                        ? Preprocess.autoLayout(sourceGray, sourceH, p.threshold, p.maxGap)
                        : Preprocess.compressGrayGaps(toGray(src, "fit"),
                                p.threshold, p.maxGap);
            }
        }
        boolean[][] mono = p.dither
                ? Preprocess.dither(gray, p.threshold)
                : Preprocess.threshold(gray, p.threshold);
        return Framebuffer.pack(mono);
    }

    private boolean cacheActive() {
        return prefs().cacheEnabled && !liveFallback;
    }

    private static boolean eligibleReaderPackage(String pkg) {
        return pkg != null && !pkg.isEmpty()
                && !pkg.equals("android")
                && !pkg.equals("com.murphy.m4screenbridge")
                && !pkg.startsWith("com.android.");
    }

    private void setLiveFallback(boolean value) {
        if (liveFallback == value) return;
        liveFallback = value;
        store.reset();
        wakeLoop();
        notifyStatus();
    }

    private static int[] toGrayAtWidth(Bitmap src, int outH) {
        Bitmap canvas = Bitmap.createBitmap(Preprocess.W, outH, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(canvas);
        c.drawColor(Color.WHITE);
        Paint paint = new Paint();
        paint.setFilterBitmap(true);
        c.drawBitmap(src, new Rect(0, 0, src.getWidth(), src.getHeight()),
                new Rect(0, 0, Preprocess.W, outH), paint);
        int[] px = new int[Preprocess.W * outH];
        canvas.getPixels(px, 0, Preprocess.W, 0, 0, Preprocess.W, outH);
        canvas.recycle();
        return rgbToGray(px);
    }

    private static int[] toGray(Bitmap src, String cropMode) {
        int sw = src.getWidth(), sh = src.getHeight();
        Bitmap canvas = Bitmap.createBitmap(Preprocess.W, Preprocess.H, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(canvas);
        c.drawColor(Color.WHITE);
        Paint paint = new Paint();
        paint.setFilterBitmap(true);
        float sx = (float) Preprocess.W / sw;
        float sy = (float) Preprocess.H / sh;
        float scale = "cover".equals(cropMode) ? Math.max(sx, sy) : Math.min(sx, sy);
        int dw = Math.round(sw * scale);
        int dh = Math.round(sh * scale);
        int left = (Preprocess.W - dw) / 2;
        int top = (Preprocess.H - dh) / 2;
        c.drawBitmap(src, new Rect(0, 0, sw, sh), new Rect(left, top, left + dw, top + dh), paint);
        int[] px = new int[Preprocess.W * Preprocess.H];
        canvas.getPixels(px, 0, Preprocess.W, 0, 0, Preprocess.W, Preprocess.H);
        canvas.recycle();
        return rgbToGray(px);
    }

    private static int[] rgbToGray(int[] px) {
        int[] gray = new int[px.length];
        for (int i = 0; i < px.length; i++) {
            int col = px[i];
            int r = (col >> 16) & 0xFF;
            int g = (col >> 8) & 0xFF;
            int b = col & 0xFF;
            gray[i] = (30 * r + 59 * g + 11 * b) / 100;
        }
        return gray;
    }

    private Prefs prefs() {
        return new Prefs(getSharedPreferences(PREFS, Context.MODE_PRIVATE));
    }
}
