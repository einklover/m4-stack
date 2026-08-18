package com.murphy.m4screenbridge.browser;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.Process;

import com.murphy.m4screenbridge.MainActivity;
import com.murphy.m4screenbridge.R;

import java.io.FileDescriptor;
import java.io.PrintWriter;

/**
 * M1 owner for the virtual browser lifecycle. MainActivity is intentionally only a controller;
 * closing/recreating the activity must not tear down the WebView/VirtualDisplay session.
 */
public final class BrowserBridgeService extends Service {
    private static final String ACTION_START_URL = "com.murphy.m4screenbridge.browser.START_URL";
    private static final String ACTION_SELF_TEST = "com.murphy.m4screenbridge.browser.SELF_TEST";
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
            stopSessionAndSelf();
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
        } else if (ACTION_START_URL.equals(action)) {
            session.start(intent == null ? null : intent.getStringExtra(EXTRA_URL));
            acquireWakeLock();
        }
        updateNotification();
        return START_STICKY;
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

    public static void startJavaScriptSelfTest(Context context) {
        Intent i = new Intent(context, BrowserBridgeService.class);
        i.setAction(ACTION_SELF_TEST);
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
            context.stopService(new Intent(context, BrowserBridgeService.class));
        }
    }

    public static boolean isActive() {
        BrowserBridgeService s = instance;
        return s != null && s.session != null && s.session.isActive();
    }

    public static String snapshot() {
        BrowserBridgeService s = instance;
        if (s == null || s.session == null) return "虚拟浏览器 M1：前台服务未启动";
        return "FGS：运行中 pid=" + Process.myPid() + "\n" + s.session.snapshot();
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
                }
            }
        }
        writer.println(snapshot());
    }

    private void applyHostExtras(Intent intent) {
        if (intent == null || session == null) return;
        String host = intent.getStringExtra(EXTRA_HOST);
        int port = intent.getIntExtra(EXTRA_PORT, -1);
        if (host != null || port > 0) session.applyHostOverride(host, port);
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

    private void stopSessionAndSelf() {
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

        String text = session != null && session.isActive()
                ? "480×800 virtual browser is running"
                : "Browser bridge service is ready";
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
