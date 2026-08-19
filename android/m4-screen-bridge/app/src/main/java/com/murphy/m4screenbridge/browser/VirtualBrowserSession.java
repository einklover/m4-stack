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
import android.view.InputDevice;
import android.view.MotionEvent;

import android.util.Log;

import com.murphy.m4screenbridge.browser.patch.ExtraDimCompensation;
import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.RgbaFrameProbe;
import com.murphy.m4screenbridge.Prefs;
import com.murphy.m4screenbridge.ScreenBridgeService;
import com.murphy.m4screenbridge.browser.stream.M4B3;
import com.murphy.m4screenbridge.browser.stream.M4B3Codec;
import com.murphy.m4screenbridge.browser.stream.M4B3InputState;
import com.murphy.m4screenbridge.browser.stream.M4B3KeyState;
import com.murphy.m4screenbridge.browser.stream.M4B3Message;
import com.murphy.m4screenbridge.browser.stream.M4B3ReferenceReceiver;
import com.murphy.m4screenbridge.browser.stream.M4B3Sender;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery;
import com.murphy.m4screenbridge.browser.discovery.NsdM4Discovery;
import com.murphy.m4screenbridge.browser.stream.M4B3TcpTransport;

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
    private M4B3TcpTransport tcpTransport;
    private String transportMode = "none";
    private String transportEndpoint = "";
    private volatile boolean transportConnected;
    private volatile long transportReconnects;
    private volatile String transportError = "";
    private boolean helloStarted;
    private M4LanDiscovery.Engine discoveryEngine;
    private NsdM4Discovery nsdDiscovery;
    private volatile String discoverySnap = "";
    private volatile long discoveryWaitFrames;

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

    private final M4B3InputState inputState = new M4B3InputState();
    private final M4B3KeyState keyState = new M4B3KeyState();
    private volatile long inputDispatched;
    private volatile String inputSnap = "";
    private volatile long keyDispatched;
    private volatile long keyUnhandled;
    private final Runnable discoveryTick = this::tickDiscovery;

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
        inputState.reset();
        keyState.reset();
        inputDispatched = 0;
        inputSnap = "";
        keyDispatched = 0;
        keyUnhandled = 0;
        transportConnected = false;
        transportReconnects = 0;
        transportError = "";
        helloStarted = false;
        discoveryWaitFrames = 0;
        discoverySnap = "";

        try {
            protocolThread = new HandlerThread("m4-browser-m4b3");
            protocolThread.start();
            protocolHandler = new Handler(protocolThread.getLooper());
            startTransport();

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

    /** Asymmetric full-frame landmark page for physical panel orientation proof. */
    public static String landmarkHtml() {
        return "<!doctype html><html><head>"
                + "<meta name=viewport content='width=480,initial-scale=1,user-scalable=no'>"
                + "<style>"
                + "html,body{margin:0;padding:0;background:#ffffff;color:#ffffff;"
                + "width:480px;height:800px;overflow:hidden;font:bold 14px sans-serif}"
                + ".box{position:absolute;background:#000000}"
                + ".lab{position:absolute;color:#ffffff;font:bold 18px sans-serif;z-index:2}"
                + "</style></head><body>"
                + "<div class=box style='left:0;top:0;width:64px;height:64px'></div>"
                + "<div class=lab style='left:8px;top:20px'>TL</div>"
                + "<div class=box style='left:448px;top:0;width:32px;height:64px'></div>"
                + "<div class=lab style='left:450px;top:20px'>TR</div>"
                + "<div class=box style='left:0;top:768px;width:64px;height:32px'></div>"
                + "<div class=lab style='left:8px;top:770px'>BL</div>"
                + "<div class=box style='left:432px;top:752px;width:48px;height:48px'></div>"
                + "<div class=lab style='left:436px;top:764px'>BR</div>"
                + "<div class=box style='left:40px;top:80px;width:80px;height:40px'></div>"
                + "<div class=lab style='left:68px;top:90px'>A</div>"
                + "<div class=box style='left:200px;top:200px;width:40px;height:80px'></div>"
                + "<div class=lab style='left:212px;top:230px'>B</div>"
                + "<div class=box style='left:80px;top:500px;width:120px;height:24px'></div>"
                + "<div class=lab style='left:128px;top:502px'>C</div>"
                + "</body></html>";
    }

    public void startLandmarkTest() {
        start("data:text/html;charset=utf-8," + Uri.encode(landmarkHtml()));
    }

    public static String inputTestHtml() {
        StringBuilder lines = new StringBuilder();
        for (int i = 0; i < 40; i++) lines.append("line ").append(i).append("<br>");
        return "<!doctype html><html><head>"
                + "<meta name=viewport content='width=480,initial-scale=1,user-scalable=no'>"
                + "<style>"
                + "html,body{margin:0;padding:0;background:#ffffff;color:#000000;"
                + "width:480px;height:800px;overflow:hidden;font:14px sans-serif}"
                + ".t{position:absolute;background:#000;color:#fff;display:flex;"
                + "align-items:center;justify-content:center;font-weight:bold}"
                + "#btnA{position:absolute;left:168px;top:72px;width:144px;height:48px;"
                + "background:#000;color:#fff;font:bold 16px sans-serif;border:0}"
                + "#scroll{position:absolute;left:16px;top:520px;width:200px;height:200px;"
                + "overflow-y:scroll;background:#eee;border:2px solid #000}"
                + "#drag{position:absolute;left:240px;top:560px;width:220px;height:80px;"
                + "background:#000;color:#fff;display:flex;align-items:center;justify-content:center}"
                + "#lp{position:absolute;left:240px;top:650px;width:220px;height:80px;"
                + "background:#444;color:#fff;display:flex;align-items:center;justify-content:center}"
                + "#hud{position:absolute;left:8px;top:250px;width:464px;height:250px;"
                + "font:12px monospace;white-space:pre-wrap}"
                + "</style></head><body>"
                + "<div class=t id=TL style='left:0;top:0;width:48px;height:48px'>TL</div>"
                + "<div class=t id=TR style='left:432px;top:0;width:48px;height:48px'>TR</div>"
                + "<div class=t id=BL style='left:0;top:752px;width:48px;height:48px'>BL</div>"
                + "<div class=t id=BR style='left:432px;top:752px;width:48px;height:48px'>BR</div>"
                + "<div class=t id=CTR style='left:216px;top:376px;width:48px;height:48px'>CTR</div>"
                + "<div class=t id=A style='left:64px;top:140px;width:72px;height:36px'>A</div>"
                + "<div class=t id=B style='left:300px;top:420px;width:40px;height:72px'>B</div>"
                + "<button id=btnA>BUTTON A 0</button>"
                + "<div id=scroll><div id=si>" + lines + "</div></div>"
                + "<div id=drag>DRAG</div><div id=lp>LONG 0</div><div id=hud></div>"
                + "<script>"
                + "let buttonA=0,down=0,move=0,up=0,cancel=0,lp=0,lpT=0,last='';"
                + "const boxes=["
                + "['TL',0,0,48,48],['TR',432,0,48,48],['BL',0,752,48,48],['BR',432,752,48,48],"
                + "['CTR',216,376,48,48],['A',64,140,72,36],['B',300,420,40,72],"
                + "['BTN_A',168,72,144,48],['SCROLL',16,520,200,200],['DRAG',240,560,220,80],"
                + "['LP',240,650,220,80]];"
                + "function hit(x,y){for(let i=0;i<boxes.length;i++){const b=boxes[i];"
                + "if(x>=b[1]&&x<b[1]+b[3]&&y>=b[2]&&y<b[2]+b[4])return b[0];}return '';}"
                + "function hud(){const s=document.getElementById('scroll').scrollTop;"
                + "const t='btnA='+buttonA+' d/m/u/c='+down+'/'+move+'/'+up+'/'+cancel"
                + "+' scrollY='+s+' lp='+lp+'\\n'+last;"
                + "document.getElementById('hud').textContent=t;"
                + "if(window.M4Input)M4Input.report(last.split(' ')[0]||'',"
                + "parseInt((last.split(' ')[1]||'0,0').split(',')[0],10)||0,"
                + "parseInt((last.split(' ')[1]||'0,0').split(',')[1],10)||0,"
                + "hit(parseInt((last.split(' ')[1]||'0,0').split(',')[0],10)||0,"
                + "parseInt((last.split(' ')[1]||'0,0').split(',')[1],10)||0),"
                + "buttonA,s,lp,down,move,up,cancel);}"
                + "function onPtr(ev){"
                + "const x=ev.clientX|0,y=ev.clientY|0,h=hit(x,y);"
                + "if(ev.type==='pointerdown'){down++;lpT=setTimeout(function(){lp++;"
                + "document.getElementById('lp').textContent='LONG '+lp;hud();},500);}"
                + "else if(ev.type==='pointermove'){move++;}"
                + "else if(ev.type==='pointerup'){up++;clearTimeout(lpT);"
                + "if(h==='BTN_A'){buttonA++;document.getElementById('btnA').textContent='BUTTON A '+buttonA;}}"
                + "else if(ev.type==='pointercancel'){cancel++;clearTimeout(lpT);}"
                + "last=ev.type.replace('pointer','').toUpperCase()+' '+x+','+y+' '+h;"
                + "hud();}"
                + "['pointerdown','pointermove','pointerup','pointercancel'].forEach(function(t){"
                + "document.addEventListener(t,onPtr,true);});"
                + "hud();"
                + "</script></body></html>";
    }

    public void startInputTest() {
        start("data:text/html;charset=utf-8," + Uri.encode(inputTestHtml()));
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
                .append("\nM4B3 ").append(transportMode).append(' ').append(transportEndpoint)
                .append(transportConnected ? " connected" : " disconnected")
                .append(protocolInFlight ? " in-flight" : " idle")
                .append(protocolPending ? " pending" : "")
                .append(" nack ").append(nackRecoveries)
                .append(" recon ").append(transportReconnects)
                .append(" crc ").append(M4B3.crcHex(lastAckedCrc));
        if (!transportError.isEmpty()) sb.append("\nM4B3 err ").append(transportError);
        if (!discoverySnap.isEmpty()) sb.append("\n").append(discoverySnap);
        if (discoveryWaitFrames > 0) sb.append(" waitFrames=").append(discoveryWaitFrames);
        sb.append("\n").append(inputSnap.isEmpty() ? inputState.snapshot() : inputSnap)
                .append(" dispatched=").append(inputDispatched);
        sb.append("\n").append(keyState.snapshot())
                .append(" dispatched=").append(keyDispatched)
                .append(" unhandled=").append(keyUnhandled);
        BrowserPresentation.JsProbe probe = presentation == null ? null : presentation.jsProbe();
        if (probe != null) {
            sb.append("\njs hit=").append(probe.lastHit)
                    .append(" act=").append(probe.lastAction)
                    .append(" xy=").append(probe.lastX).append(',').append(probe.lastY)
                    .append(" btnA=").append(probe.buttonA)
                    .append(" d/m/u/c=")
                    .append(probe.down).append('/').append(probe.move).append('/')
                    .append(probe.up).append('/').append(probe.cancel)
                    .append(" scrollY=").append(probe.scrollY)
                    .append(" lp=").append(probe.longPress)
                    .append(" reports=").append(probe.hits);
            if (!probe.lastLog.isEmpty()) sb.append("\njs ").append(probe.lastLog);
        }
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
        if (s == null) {
            discoveryWaitFrames++;
            return;
        }
        s.offerFrame(target);
        refreshProtocolStats();
    }

    public boolean debugInjectWrongBase() {
        M4B3Sender s = sender;
        return s != null && s.debugInjectWrongBase();
    }

    public boolean debugInjectCorruptCrc() {
        M4B3Sender s = sender;
        return s != null && s.debugInjectCorruptCrc();
    }

    // dumpsys-only synthetic pointer. Same dispatch path as M4B3 TOUCH.
    // Not a product input source; used for unattended present-mode evidence.
    public boolean debugTap(int x, int y) {
        if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return false;
        if (!active || presentation == null) return false;
        main.post(() -> {
            long now = SystemClock.uptimeMillis();
            dispatchTouch(new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, x, y, now, 1, 1));
            dispatchTouch(new M4B3Message.Touch(M4B3.TOUCH_UP, 0, x, y, now + 16, 2, 1));
        });
        return true;
    }

    public void applyHostOverride(String host, int port) {
        android.content.SharedPreferences sp =
                hostCtx().getSharedPreferences(ScreenBridgeService.PREFS, android.content.Context.MODE_PRIVATE);
        android.content.SharedPreferences.Editor e = sp.edit();
        if (host != null) e.putString(Prefs.KEY_M4B3_HOST, host.trim());
        if (port > 0) e.putInt(Prefs.KEY_M4B3_PORT, port);
        e.apply();
    }

    private android.content.Context hostCtx() {
        return host;
    }

    private void startTransport() {
        android.content.SharedPreferences sp =
                host.getSharedPreferences(ScreenBridgeService.PREFS, android.content.Context.MODE_PRIVATE);
        String rawHost = Prefs.m4b3HostRaw(sp);
        int port = Prefs.m4b3Port(sp);
        M4LanDiscovery.HostMode mode = M4LanDiscovery.classify(rawHost);
        discoveryEngine = new M4LanDiscovery.Engine();
        if (mode == M4LanDiscovery.HostMode.MANUAL) {
            discoveryEngine.setManual(rawHost, port);
            applyDiscoveryDecision(discoveryEngine.decision(), false);
            return;
        }
        if (mode == M4LanDiscovery.HostMode.LOOPBACK) {
            discoveryEngine.setLoopback();
            applyDiscoveryDecision(discoveryEngine.decision(), false);
            return;
        }
        String cachedHost = Prefs.cachedHost(sp);
        int cachedPort = Prefs.cachedPort(sp);
        M4LanDiscovery.Endpoint cached = M4LanDiscovery.validHost(cachedHost) && M4LanDiscovery.validPort(cachedPort)
                ? new M4LanDiscovery.Endpoint("cached", cachedHost, cachedPort) : null;
        discoveryEngine.startAuto(cached, SystemClock.elapsedRealtime());
        applyDiscoveryDecision(discoveryEngine.decision(), false);
        nsdDiscovery = new NsdM4Discovery(host, new NsdM4Discovery.Listener() {
            @Override
            public void onResolved(M4LanDiscovery.Endpoint endpoint) {
                M4LanDiscovery.Engine eng = discoveryEngine;
                if (eng == null) return;
                eng.onResolved(endpoint, SystemClock.elapsedRealtime());
                applyDiscoveryDecision(eng.decision(), true);
                NsdM4Discovery nsd = nsdDiscovery;
                if (nsd != null) nsd.requestRestartIfNeeded(eng);
            }

            @Override
            public void onLost(String lostHost, int lostPort) {
                M4LanDiscovery.Engine eng = discoveryEngine;
                if (eng == null) return;
                eng.onLost(lostHost, lostPort, SystemClock.elapsedRealtime());
                applyDiscoveryDecision(eng.decision(), true);
            }

            @Override
            public void onError(String message) {
                M4LanDiscovery.Engine eng = discoveryEngine;
                if (eng == null) return;
                eng.onError(message, SystemClock.elapsedRealtime());
                discoverySnap = eng.snapshot();
            }
        });
        nsdDiscovery.start();
        main.removeCallbacks(discoveryTick);
        main.postDelayed(discoveryTick, 1000);
    }

    private void tickDiscovery() {
        M4LanDiscovery.Engine eng = discoveryEngine;
        NsdM4Discovery nsd = nsdDiscovery;
        if (eng == null || eng.isStopped()) return;
        eng.tick(SystemClock.elapsedRealtime());
        if (nsd != null) nsd.requestRestartIfNeeded(eng);
        discoverySnap = eng.snapshot();
        main.postDelayed(discoveryTick, 1000);
    }

    private void applyDiscoveryDecision(M4LanDiscovery.Decision d, boolean persistCache) {
        if (d == null) return;
        discoverySnap = discoveryEngine == null ? "" : discoveryEngine.snapshot();
        if (d.source == M4LanDiscovery.Source.LOOPBACK) {
            startLoopbackTransport();
            return;
        }
        if (d.source == M4LanDiscovery.Source.NONE) {
            stopTcpOnly();
            transportMode = "none";
            transportEndpoint = "";
            transportConnected = false;
            return;
        }
        if (!d.hasEndpoint()) {
            stopTcpOnly();
            transportMode = "none";
            transportEndpoint = "";
            return;
        }
        if (persistCache && d.source == M4LanDiscovery.Source.DISCOVERED) {
            android.content.SharedPreferences sp =
                    host.getSharedPreferences(ScreenBridgeService.PREFS, android.content.Context.MODE_PRIVATE);
            Prefs.storeCachedEndpoint(sp, d.endpoint.host, d.endpoint.port);
        }
        startTcpTransport(d.endpoint.host, d.endpoint.port, d.source.name().toLowerCase(Locale.ROOT));
    }

    private void startLoopbackTransport() {
        if ("loopback".equals(transportMode) && sender != null && tcpTransport == null) return;
        stopTcpOnly();
        transportMode = "loopback";
        transportEndpoint = "loopback";
        transportConnected = true;
        localReceiver = new M4B3ReferenceReceiver();
        sender = new M4B3Sender(packet -> {
            Handler h = protocolHandler;
            if (h != null) h.post(() -> deliverLoopback(packet));
        });
        sender.connect();
        helloStarted = true;
    }

    private void startTcpTransport(String hostName, int port, String modeLabel) {
        String ep = hostName + ":" + port;
        if (tcpTransport != null && ep.equals(transportEndpoint) && sender != null) {
            transportMode = modeLabel;
            return;
        }
        stopTcpOnly();
        transportMode = modeLabel;
        transportEndpoint = ep;
        tcpTransport = new M4B3TcpTransport(protocolHandler, new M4B3TcpTransport.Listener() {
            @Override
            public void onConnected(String endpoint) {
                transportEndpoint = endpoint;
                transportConnected = true;
                transportReconnects = tcpTransport == null ? transportReconnects : tcpTransport.reconnects();
                transportError = "";
                M4B3Sender s = sender;
                if (s == null) return;
                if (!helloStarted) {
                    s.connect();
                    helloStarted = true;
                } else {
                    s.reconnect();
                }
                refreshProtocolStats();
            }

            @Override
            public void onDisconnected(String reason) {
                transportConnected = false;
                if (!reason.isEmpty()) transportError = reason;
                M4B3Sender s = sender;
                if (s != null) s.noteTransportLost();
                main.post(() -> {
                    cancelActivePointer("tcp-disconnect");
                    keyState.onTransportLost();
                });
                refreshProtocolStats();
            }

            @Override
            public void onReply(byte[] packet) {
                handleInbound(packet);
            }

            @Override
            public void onError(String message) {
                transportError = message == null ? "" : message;
            }
        });
        sender = new M4B3Sender(tcpTransport);
        tcpTransport.start(hostName, port);
    }

    private void stopTcpOnly() {
        if (tcpTransport != null) {
            tcpTransport.stop();
            tcpTransport = null;
        }
        if (sender != null) {
            sender.disconnect();
            sender = null;
        }
        localReceiver = null;
        helloStarted = false;
        transportConnected = false;
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
        cancelActivePointer("session-stop");
        keyState.onTransportLost();
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
        main.removeCallbacks(discoveryTick);
        if (nsdDiscovery != null) {
            nsdDiscovery.stop();
            nsdDiscovery = null;
        }
        if (discoveryEngine != null) {
            discoveryEngine.stop();
            discoverySnap = discoveryEngine.snapshot();
            discoveryEngine = null;
        }
        stopTcpOnly();
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

    private void handleInbound(byte[] packet) {
        M4B3Message msg;
        try {
            msg = M4B3Codec.parse(packet);
        } catch (Throwable t) {
            applyErrors++;
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
            Log.e(TAG, "M4B3 tcp parse failed: " + error, t);
            return;
        }
        if (msg.type == M4B3.TYPE_TOUCH) {
            main.post(() -> dispatchTouch(msg.touch));
            return;
        }
        if (msg.type == M4B3.TYPE_INPUT_KEY) {
            main.post(() -> dispatchInputKey(msg.inputKey));
            return;
        }
        M4B3Sender s = sender;
        if (s == null) return;
        try {
            s.receive(packet);
            refreshProtocolStats();
        } catch (Throwable t) {
            applyErrors++;
            error = t.getClass().getSimpleName() + ": " + safeMessage(t);
            Log.e(TAG, "M4B3 tcp reply failed: " + error, t);
        }
    }

    private void dispatchInputKey(M4B3Message.InputKey key) {
        if (!keyState.accept(key)) return;
        BrowserPresentation p = presentation;
        boolean handled = false;
        if (p != null && key.action == M4B3.INPUT_KEY_BACK) {
            handled = p.goBackInBrowser();
        } else if (p != null && key.action == M4B3.INPUT_KEY_RELOAD) {
            handled = p.reloadBrowser();
        }
        if (handled) keyDispatched++;
        else keyUnhandled++;
        Log.i(TAG, String.format(Locale.ROOT,
                "key %s seq=%d sess=%d handled=%d dispatched=%d unhandled=%d",
                key.action == M4B3.INPUT_KEY_BACK ? "BACK" : "RELOAD",
                key.inputSeq, key.session, handled ? 1 : 0, keyDispatched, keyUnhandled));
    }

    private void dispatchTouch(M4B3Message.Touch touch) {
        List<M4B3InputState.Dispatch> events =
                inputState.apply(touch, SystemClock.uptimeMillis());
        for (int i = 0; i < events.size(); i++) {
            emitMotion(events.get(i));
        }
        inputSnap = inputState.snapshot();
    }

    private void cancelActivePointer(String why) {
        M4B3InputState.Dispatch cancel = inputState.onTransportLost(SystemClock.uptimeMillis());
        if (cancel != null) {
            emitMotion(cancel);
            Log.i(TAG, "input CANCEL synthesized (" + why + ")");
        }
        inputSnap = inputState.snapshot();
    }

    private void emitMotion(M4B3InputState.Dispatch d) {
        MotionEvent ev = MotionEvent.obtain(d.downTime, d.eventTime, d.motionAction,
                (float) d.x, (float) d.y, 0);
        ev.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        BrowserPresentation p = presentation;
        boolean ok = p != null && p.dispatchBrowserTouch(ev);
        ev.recycle();
        if (ok) inputDispatched++;
        String name = d.motionAction == M4B3InputState.ACTION_DOWN ? "DOWN"
                : d.motionAction == M4B3InputState.ACTION_MOVE ? "MOVE"
                : d.motionAction == M4B3InputState.ACTION_UP ? "UP" : "CANCEL";
        Log.i(TAG, String.format(Locale.ROOT,
                "input %s xy=%d,%d seq=%d sess=%d synth=%d dispatched=%d ok=%d",
                name, d.x, d.y, d.inputSeq, d.session, d.synthesized ? 1 : 0,
                inputDispatched, ok ? 1 : 0));
    }

    private static String safeMessage(Throwable t) {
        String msg = t.getMessage();
        return msg == null ? "" : msg;
    }
}
