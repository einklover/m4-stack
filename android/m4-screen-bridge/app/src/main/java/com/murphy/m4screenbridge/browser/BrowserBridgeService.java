package com.murphy.m4screenbridge.browser;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Process;
import android.util.Log;

import com.murphy.m4screenbridge.MainActivity;
import com.murphy.m4screenbridge.Prefs;
import com.murphy.m4screenbridge.R;
import com.murphy.m4screenbridge.ScreenBridgeService;
import com.murphy.m4screenbridge.browser.session.BrowserConnectionState;

import java.io.FileDescriptor;
import java.io.PrintWriter;

/**
 * Product owner for the virtual browser lifecycle. MainActivity is intentionally only a controller;
 * closing/recreating the activity must not tear down the WebView/VirtualDisplay session.
 */
public final class BrowserBridgeService extends Service {
    private static final String TAG = "M4BrowserService";
    private static final String ACTION_START_URL = "com.murphy.m4screenbridge.browser.START_URL";
    private static final String ACTION_RESUME = "com.murphy.m4screenbridge.browser.RESUME";
    private static final String ACTION_SELF_TEST = "com.murphy.m4screenbridge.browser.SELF_TEST";
    private static final String ACTION_LANDMARK = "com.murphy.m4screenbridge.browser.LANDMARK";
    private static final String ACTION_INPUT_TEST = "com.murphy.m4screenbridge.browser.INPUT_TEST";
    private static final String ACTION_STOP = "com.murphy.m4screenbridge.browser.STOP";
    private static final String ACTION_INJECT_BASE = "com.murphy.m4screenbridge.browser.INJECT_BASE";
    private static final String ACTION_INJECT_CRC = "com.murphy.m4screenbridge.browser.INJECT_CRC";
    private static final String EXTRA_URL = "url";
    public static final String EXTRA_HOST = "m4_host";
    public static final String EXTRA_PORT = "m4_port";
    private static final String CHANNEL_ID = "m4_browser_bridge";
    private static final int NOTIFICATION_ID = 48625;

    private static volatile BrowserBridgeService instance;
    private VirtualBrowserSession session;
    private PowerManager.WakeLock wakeLock;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        session = new VirtualBrowserSession(this);
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopSessionAndSelf(true);
            return START_NOT_STICKY;
        }

        // A sticky restart may deliver a null Intent. Do not create an idle foreground service when
        // the user has explicitly disabled resume or there is no restorable product URL.
        if ((action == null || ACTION_RESUME.equals(action)) && !hasRestorableProductSession(prefs())) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf(startId);
            return START_NOT_STICKY;
        }

        enterForeground();
        applyHostExtras(intent);
        if (ACTION_INJECT_BASE.equals(action)) {
            if (session != null) session.debugInjectWrongBase();
        } else if (ACTION_INJECT_CRC.equals(action)) {
            if (session != null) session.debugInjectCorruptCrc();
        } else if (ACTION_SELF_TEST.equals(action)) {
            session.startJavaScriptSelfTest();
            acquireWakeLock();
        } else if (ACTION_LANDMARK.equals(action)) {
            session.startLandmarkTest();
            acquireWakeLock();
        } else if (ACTION_INPUT_TEST.equals(action)) {
            session.startInputTest();
            acquireWakeLock();
        } else if (ACTION_START_URL.equals(action)) {
            SharedPreferences sp = prefs();
            Prefs.setBrowserResumeEnabled(sp, true);
            session.start(intent == null ? null : intent.getStringExtra(EXTRA_URL));
            Prefs.storeBrowserLastUrl(sp, session.currentUrl());
            acquireWakeLock();
        } else if (action == null || ACTION_RESUME.equals(action)) {
            if (!restoreProductSessionIfConfigured()) {
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf(startId);
                return START_NOT_STICKY;
            }
        }
        updateNotification();
        return Prefs.browserResumeEnabled(prefs()) ? START_STICKY : START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        releaseWakeLock();
        if (session != null) session.stop();
        session = null;
        if (instance == this) instance = null;
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    public static void startUrl(Context context, String url) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_START_URL);
        i.putExtra(EXTRA_URL, url);
        context.startForegroundService(i);
    }

    /**
     * Resume only a session the user previously enabled. Safe to call from Activity cold-start,
     * BOOT_COMPLETED, package replacement, or a controller that does not know whether the FGS lives.
     */
    public static boolean resumeIfConfigured(Context context) {
        if (context == null) return false;
        BrowserBridgeService running = instance;
        if (running != null && running.session != null && running.session.isActive()) return true;
        SharedPreferences sp = context.getSharedPreferences(
                ScreenBridgeService.PREFS, Context.MODE_PRIVATE);
        if (!hasRestorableProductSession(sp)) return false;
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_RESUME);
        try {
            context.startForegroundService(i);
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "resume start rejected: " + e.getClass().getSimpleName() + ": "
                    + (e.getMessage() == null ? "" : e.getMessage()));
            return false;
        }
    }

    public static void startJavaScriptSelfTest(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_SELF_TEST);
        context.startForegroundService(i);
    }

    public static void startLandmarkTest(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_LANDMARK);
        context.startForegroundService(i);
    }

    public static void startInputTest(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_INPUT_TEST);
        context.startForegroundService(i);
    }

    public static void injectWrongBase(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_INJECT_BASE);
        context.startService(i);
    }

    public static void injectCorruptCrc(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_INJECT_CRC);
        context.startService(i);
    }

    public static void stop(Context context) {
        BrowserBridgeService s = instance;
        if (s != null) {
            Intent i = new Intent(context, BrowserBridgeService.class);
            i.setAction(ACTION_STOP);
            context.startService(i);
        } else {
            SharedPreferences sp = context.getSharedPreferences(
                    ScreenBridgeService.PREFS, Context.MODE_PRIVATE);
            Prefs.setBrowserResumeEnabled(sp, false);
            context.stopService(new Intent(context, BrowserBridgeService.class));
        }
    }

    public static boolean isActive() {
        BrowserBridgeService s = instance;
        return s != null && s.session != null && s.session.isActive();
    }

    public static String snapshot() {
        BrowserBridgeService s = instance;
        if (s == null || s.session == null) return "虚拟浏览器 M5：前台服务未启动";
        return "FGS：运行中 pid=" + Process.myPid() + "\n" + s.session.snapshot();
    }

    /** Compact status rendered inside the M4 VirtualDisplay. Empty means no overlay. */
    static String bridgeOverlayStatus() {
        BrowserBridgeService s = instance;
        if (s == null || s.session == null || !s.session.isActive()) return "";
        BrowserConnectionState.Snapshot state = s.session.connectionSnapshot();
        switch (state.state) {
            case DISCOVERING:
                return "正在查找 M4…";
            case CONNECTING:
                return state.endpoint().isEmpty()
                        ? "正在连接 M4…" : "正在连接 M4… " + state.endpoint();
            case RECONNECTING:
                return "M4 已断开，正在重连…";
            case ERROR:
                return state.error.isEmpty() ? "M4 连接失败" : "M4 连接失败：" + state.error;
            case CONNECTED:
            case DISABLED:
            default:
                return "";
        }
    }

    @Override
    protected void dump(FileDescriptor fd, PrintWriter writer, String[] args) {
        writer.println("BrowserBridgeService");
        writer.println("pid=" + Process.myPid());
        if (args != null) {
            for (String arg : args) {
                if ("inject-base".equals(arg) && session != null) {
                    writer.println("inject-base=" + session.debugInjectWrongBase());
                } else if ("inject-crc".equals(arg) && session != null) {
                    writer.println("inject-crc=" + session.debugInjectCorruptCrc());
                } else if (arg.startsWith("tap=") && session != null) {
                    String[] xy = arg.substring(4).split(",");
                    boolean ok = false;
                    int x = -1;
                    int y = -1;
                    if (xy.length == 2) {
                        try {
                            x = Integer.parseInt(xy[0].trim());
                            y = Integer.parseInt(xy[1].trim());
                            ok = session.debugTap(x, y);
                        } catch (NumberFormatException ignored) {
                            ok = false;
                        }
                    }
                    writer.println("tap=" + ok + " " + x + "," + y);
                }
            }
        }
        writer.println("resumeEnabled=" + Prefs.browserResumeEnabled(prefs()));
        writer.println("lastUrl=" + Prefs.browserLastUrl(prefs()));
        writer.println(snapshot());
    }

    private void applyHostExtras(Intent intent) {
        if (intent == null || session == null) return;
        String host = intent.getStringExtra(EXTRA_HOST);
        int port = intent.getIntExtra(EXTRA_PORT, -1);
        if (host != null || port > 0) session.applyHostOverride(host, port);
    }

    private boolean restoreProductSessionIfConfigured() {
        if (session == null) return false;
        if (session.isActive()) return true;
        SharedPreferences sp = prefs();
        if (!hasRestorableProductSession(sp)) return false;
        session.start(Prefs.browserLastUrl(sp));
        if (!session.isActive()) return false;
        acquireWakeLock();
        return true;
    }

    private static boolean hasRestorableProductSession(SharedPreferences sp) {
        return Prefs.browserResumeEnabled(sp) && !Prefs.browserLastUrl(sp).isEmpty();
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(ScreenBridgeService.PREFS, MODE_PRIVATE);
    }

    private void acquireWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) return;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm == null) return;
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "m4screenbridge:m4b3");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        wakeLock = null;
    }

    private void stopSessionAndSelf(boolean disableResume) {
        if (disableResume) Prefs.setBrowserResumeEnabled(prefs(), false);
        releaseWakeLock();
        if (session != null) session.stop();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void createNotificationChannel() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                "M4 E-ink Browser Bridge", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps the app-owned M4 virtual browser display alive");
        nm.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent contentIntent = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent stop = new Intent(this, BrowserBridgeService.class);
        stop.setAction(ACTION_STOP);
        PendingIntent stopIntent = PendingIntent.getService(this, 1, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        String text = "Browser bridge service is ready";
        if (session != null && session.isActive()) {
            text = session.productStatusLine();
            if (text.length() > 160) text = text.substring(0, 157) + "...";
        }
        return new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle("M4 E-ink Browser")
                .setContentText(text)
                .setContentIntent(contentIntent)
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .addAction(new Notification.Action.Builder(null, "Stop", stopIntent).build())
                .build();
    }

    private void enterForeground() {
        Notification n = buildNotification();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
    }

    private void updateNotification() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTIFICATION_ID, buildNotification());
    }
}
