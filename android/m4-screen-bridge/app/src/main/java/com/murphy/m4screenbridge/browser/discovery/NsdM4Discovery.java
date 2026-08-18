package com.murphy.m4screenbridge.browser.discovery;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Map;

/**
 * Lifecycle-bounded NsdManager listener. One discovery, one in-flight resolve,
 * bounded queue. {@link #stop()} drops the listener, multicast lock, and queue.
 */
public final class NsdM4Discovery {
    public interface Listener {
        void onResolved(M4LanDiscovery.Endpoint endpoint);
        void onLost(String host, int port);
        void onError(String message);
    }

    private static final String TAG = "M4LanNsd";
    private static final int MAX_RESOLVE_Q = 8;

    private final NsdManager nsd;
    private final WifiManager wifi;
    private final Handler handler;
    private final Listener listener;

    private WifiManager.MulticastLock multicastLock;
    private NsdManager.DiscoveryListener discoveryListener;
    private boolean running;
    private boolean resolving;
    private int startAttempts;
    private String lastError = "";
    private final ArrayDeque<NsdServiceInfo> resolveQ = new ArrayDeque<NsdServiceInfo>();

    public NsdM4Discovery(Context context, Listener listener) {
        if (context == null) throw new IllegalArgumentException("context is null");
        if (listener == null) throw new IllegalArgumentException("listener is null");
        this.nsd = (NsdManager) context.getApplicationContext().getSystemService(Context.NSD_SERVICE);
        this.wifi = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        this.handler = new Handler(Looper.getMainLooper());
        this.listener = listener;
    }

    public void start() {
        stop();
        running = true;
        startAttempts = 0;
        acquireLock();
        startDiscover();
    }

    public void stop() {
        running = false;
        resolving = false;
        resolveQ.clear();
        handler.removeCallbacksAndMessages(null);
        NsdManager.DiscoveryListener d = discoveryListener;
        discoveryListener = null;
        if (d != null && nsd != null) {
            try {
                nsd.stopServiceDiscovery(d);
            } catch (Throwable ignored) {
            }
        }
        releaseLock();
    }

    public boolean isRunning() {
        return running;
    }

    public String lastError() {
        return lastError;
    }

    public void requestRestartIfNeeded(M4LanDiscovery.Engine engine) {
        if (!running || engine == null || !engine.consumeDiscoverRestart()) return;
        restartDiscover();
    }

    private void startDiscover() {
        if (!running) return;
        if (nsd == null) {
            noteError("nsd-unavailable");
            return;
        }
        startAttempts++;
        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                handler.post(() -> {
                    noteError("start-failed:" + errorCode);
                    scheduleRetry();
                });
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                handler.post(() -> noteError("stop-failed:" + errorCode));
            }

            @Override
            public void onDiscoveryStarted(String serviceType) {
                lastError = "";
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                handler.post(() -> enqueueResolve(serviceInfo));
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                handler.post(() -> handleLost(serviceInfo));
            }
        };
        try {
            nsd.discoverServices(M4LanDiscovery.SERVICE_TYPE_DOT, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
        } catch (Throwable t) {
            noteError(t.getClass().getSimpleName());
            scheduleRetry();
        }
    }

    private void restartDiscover() {
        if (!running) return;
        NsdManager.DiscoveryListener d = discoveryListener;
        discoveryListener = null;
        if (d != null && nsd != null) {
            try {
                nsd.stopServiceDiscovery(d);
            } catch (Throwable ignored) {
            }
        }
        resolving = false;
        resolveQ.clear();
        startDiscover();
    }

    private void scheduleRetry() {
        if (!running) return;
        if (startAttempts > M4LanDiscovery.MAX_DISCOVER_RESTARTS) return;
        long delay = 1000L << Math.min(startAttempts - 1, 2);
        handler.postDelayed(this::restartDiscover, delay);
    }

    private void enqueueResolve(NsdServiceInfo info) {
        if (!running || info == null) return;
        String type = M4LanDiscovery.normalizeType(info.getServiceType());
        if (!M4LanDiscovery.SERVICE_TYPE.equals(type)
                && !M4LanDiscovery.SERVICE_TYPE.equals(M4LanDiscovery.normalizeType(info.getServiceName()))) {
            if (!type.isEmpty() && !M4LanDiscovery.SERVICE_TYPE.equals(type)) return;
        }
        if (resolveQ.size() >= MAX_RESOLVE_Q) {
            noteError("resolve-queue-full");
            return;
        }
        resolveQ.addLast(info);
        pumpResolve();
    }

    private void pumpResolve() {
        if (!running || resolving || nsd == null) return;
        NsdServiceInfo next = resolveQ.pollFirst();
        if (next == null) return;
        resolving = true;
        try {
            nsd.resolveService(next, new NsdManager.ResolveListener() {
                @Override
                public void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    handler.post(() -> {
                        resolving = false;
                        noteError("resolve-failed:" + errorCode);
                        pumpResolve();
                    });
                }

                @Override
                public void onServiceResolved(NsdServiceInfo serviceInfo) {
                    handler.post(() -> {
                        resolving = false;
                        deliverResolved(serviceInfo);
                        pumpResolve();
                    });
                }
            });
        } catch (Throwable t) {
            resolving = false;
            noteError(t.getClass().getSimpleName());
        }
    }

    private void deliverResolved(NsdServiceInfo info) {
        if (!running || info == null) return;
        InetAddress addr = info.getHost();
        String host = addr == null ? "" : addr.getHostAddress();
        if (host == null) host = "";
        String proto = txt(info, M4LanDiscovery.TXT_PROTO_KEY);
        M4LanDiscovery.Endpoint ep = M4LanDiscovery.parseRecord(
                info.getServiceName(), info.getServiceType(), host, info.getPort(), proto);
        if (ep == null) {
            noteError("invalid-record");
            return;
        }
        listener.onResolved(ep);
    }

    private void handleLost(NsdServiceInfo info) {
        if (!running || info == null) return;
        InetAddress addr = info.getHost();
        String host = addr == null ? "" : addr.getHostAddress();
        if (host == null || host.isEmpty()) return;
        listener.onLost(host, info.getPort());
    }

    private void acquireLock() {
        if (wifi == null) return;
        try {
            multicastLock = wifi.createMulticastLock("m4b3-nsd");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Throwable t) {
            noteError("multicast:" + t.getClass().getSimpleName());
        }
    }

    private void releaseLock() {
        WifiManager.MulticastLock lock = multicastLock;
        multicastLock = null;
        if (lock != null && lock.isHeld()) {
            try {
                lock.release();
            } catch (Throwable ignored) {
            }
        }
    }

    private void noteError(String message) {
        lastError = message == null ? "" : message;
        Log.w(TAG, lastError);
        if (running) listener.onError(lastError);
    }

    private static String txt(NsdServiceInfo info, String key) {
        Map<String, byte[]> attrs = info.getAttributes();
        if (attrs == null || key == null) return "";
        byte[] raw = attrs.get(key);
        if (raw == null) return "";
        return new String(raw, StandardCharsets.UTF_8);
    }
}
