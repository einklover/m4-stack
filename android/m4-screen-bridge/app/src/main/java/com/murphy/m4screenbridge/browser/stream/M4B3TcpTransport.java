package com.murphy.m4screenbridge.browser.stream;

import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayDeque;
import java.util.List;

/**
 * Real TCP transport for M4B3. Connect/read/write/reconnect stay on a dedicated
 * I/O thread. Completes fragmented/coalesced replies before delivering them.
 */
public final class M4B3TcpTransport implements M4B3Outbound {
    public interface Listener {
        void onConnected(String endpoint);
        void onDisconnected(String reason);
        void onReply(byte[] packet);
        void onError(String message);
    }

    private static final String TAG = "M4B3Tcp";
    private static final int CONNECT_TIMEOUT_MS = 4000;
    private static final int SO_TIMEOUT_MS = 200;
    private static final int MAX_BACKOFF_MS = 5000;

    private final Handler replyHandler;
    private final Listener listener;

    private final Object lock = new Object();
    private final ArrayDeque<byte[]> writeQ = new ArrayDeque<>();
    private HandlerThread ioThread;
    private volatile boolean running;
    private volatile boolean connected;
    private volatile String host = "";
    private volatile int port = 48624;
    private volatile String endpoint = "";
    private volatile long reconnects;
    private volatile long writes;
    private volatile long replies;
    private volatile String lastError = "";

    private Socket socket;
    private final M4B3Framer framer = new M4B3Framer();

    public M4B3TcpTransport(Handler replyHandler, Listener listener) {
        if (replyHandler == null) throw new IllegalArgumentException("replyHandler is null");
        if (listener == null) throw new IllegalArgumentException("listener is null");
        this.replyHandler = replyHandler;
        this.listener = listener;
    }

    public void start(String host, int port) {
        stop();
        if (host == null || host.trim().isEmpty()) {
            throw new IllegalArgumentException("M4 host is empty");
        }
        if (port <= 0 || port > 65535) throw new IllegalArgumentException("bad port " + port);
        this.host = host.trim();
        this.port = port;
        this.endpoint = this.host + ":" + this.port;
        running = true;
        connected = false;
        ioThread = new HandlerThread("m4-browser-tcp");
        ioThread.start();
        new Handler(ioThread.getLooper()).post(this::ioLoop);
    }

    public void stop() {
        running = false;
        closeSocket();
        HandlerThread t = ioThread;
        ioThread = null;
        if (t != null) {
            t.quitSafely();
            try {
                t.join(1000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        synchronized (lock) {
            writeQ.clear();
            lock.notifyAll();
        }
        framer.reset();
        connected = false;
    }

    @Override
    public void enqueue(byte[] packet) {
        if (packet == null || packet.length == 0) return;
        synchronized (lock) {
            writeQ.addLast(packet);
            writes++;
            lock.notifyAll();
        }
    }

    public boolean isConnected() {
        return connected;
    }

    public String endpoint() {
        return endpoint;
    }

    public long reconnects() {
        return reconnects;
    }

    public long writes() {
        return writes;
    }

    public long replies() {
        return replies;
    }

    public String lastError() {
        return lastError;
    }

    private void ioLoop() {
        int backoff = 250;
        while (running) {
            if (!openSocket()) {
                sleepQuiet(backoff);
                backoff = Math.min(MAX_BACKOFF_MS, backoff * 2);
                continue;
            }
            backoff = 250;
            try {
                serviceSocket();
            } catch (Throwable t) {
                noteError(t);
                notifyDisconnected(safeMessage(t));
            } finally {
                closeSocket();
            }
        }
    }

    private boolean openSocket() {
        closeSocket();
        framer.reset();
        Socket s = new Socket();
        try {
            s.setTcpNoDelay(true);
            s.setKeepAlive(true);
            s.setSoTimeout(SO_TIMEOUT_MS);
            s.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
            socket = s;
            connected = true;
            reconnects++;
            lastError = "";
            Log.i(TAG, "connected " + endpoint);
            replyHandler.post(() -> listener.onConnected(endpoint));
            return true;
        } catch (Throwable t) {
            noteError(t);
            try {
                s.close();
            } catch (Throwable ignored) {
            }
            return false;
        }
    }

    private void serviceSocket() throws Exception {
        Socket s = socket;
        if (s == null) return;
        InputStream in = s.getInputStream();
        OutputStream out = s.getOutputStream();
        byte[] readBuf = new byte[4096];
        while (running && s.isConnected() && !s.isClosed()) {
            drainWrites(out);
            int n;
            try {
                n = in.read(readBuf);
            } catch (java.net.SocketTimeoutException timeout) {
                continue;
            }
            if (n < 0) {
                notifyDisconnected("eof");
                return;
            }
            if (n == 0) continue;
            List<byte[]> msgs = framer.feed(readBuf, 0, n);
            for (byte[] msg : msgs) {
                replies++;
                byte[] owned = msg;
                replyHandler.post(() -> listener.onReply(owned));
            }
        }
        if (running) notifyDisconnected("socket-closed");
    }

    private void drainWrites(OutputStream out) throws Exception {
        while (true) {
            byte[] packet;
            synchronized (lock) {
                packet = writeQ.pollFirst();
            }
            if (packet == null) return;
            out.write(packet);
            out.flush();
        }
    }

    private void closeSocket() {
        connected = false;
        Socket s = socket;
        socket = null;
        if (s != null) {
            try {
                s.close();
            } catch (Throwable ignored) {
            }
        }
    }

    private void notifyDisconnected(String reason) {
        if (!running && reason == null) return;
        Log.i(TAG, "disconnected " + endpoint + " " + reason);
        replyHandler.post(() -> listener.onDisconnected(reason == null ? "" : reason));
    }

    private void noteError(Throwable t) {
        lastError = t.getClass().getSimpleName() + ": " + safeMessage(t);
        Log.w(TAG, lastError);
        String msg = lastError;
        replyHandler.post(() -> listener.onError(msg));
    }

    private static void sleepQuiet(int ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static String safeMessage(Throwable t) {
        String m = t.getMessage();
        return m == null ? "" : m;
    }
}
