package com.murphy.m4screenbridge;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BiFunction;
import java.util.function.BooleanSupplier;
import java.util.zip.CRC32;

/** Minimal HTTP server for the M4 wire protocol on port 48624. Pure Java.
 *
 * GET  /v1/status -> JSON. GET /v1/page?index=N -> 24-byte LE header + payload.
 * POST /v1/consume?index=N -> cache acknowledgement.
 * POST /v1/tap?x=X&y=Y -> forwards a logical M4 touch in realtime mode. */
public final class HttpServer {
    public static final int PORT = 48624;

    private final PageStore store;
    private final Runnable onConsume;
    private final BiFunction<Integer, Integer, Boolean> onTap;
    private final BooleanSupplier cacheEnabled;
    private final BridgeContentApi contentApi;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final Object lock = new Object();
    private ServerSocket socket;
    private Thread acceptThread;
    private volatile long lastRequestMs = 0;
    private volatile String startError = null;

    public HttpServer(PageStore store, Runnable onConsume,
                      BiFunction<Integer, Integer, Boolean> onTap,
                      BooleanSupplier cacheEnabled, BridgeContentApi contentApi) {
        this.store = store;
        this.onConsume = onConsume;
        this.onTap = onTap;
        this.cacheEnabled = cacheEnabled;
        this.contentApi = contentApi;
    }

    public boolean connected() {
        return System.currentTimeMillis() - lastRequestMs < 10000;
    }

    public String startError() {
        return startError;
    }

    public void start() {
        synchronized (lock) {
            if (running.get()) return;
            running.set(true);
            startError = null;
            acceptThread = new Thread(this::acceptLoop, "m4-http");
            acceptThread.setDaemon(true);
            acceptThread.start();
        }
    }

    public void stop() {
        running.set(false);
        synchronized (lock) {
            if (socket != null) {
                try { socket.close(); } catch (IOException ignored) { }
                socket = null;
            }
        }
        if (acceptThread != null) {
            acceptThread.interrupt();
            acceptThread = null;
        }
    }

    private void acceptLoop() {
        try {
            socket = new ServerSocket(PORT);
        } catch (IOException e) {
            startError = "bind :" + PORT + " failed: " + e.getMessage();
            running.set(false);
            return;
        }
        while (running.get()) {
            try {
                Socket s = socket.accept();
                Thread t = new Thread(() -> handle(s), "m4-http-conn");
                t.setDaemon(true);
                t.start();
            } catch (IOException e) {
                if (running.get()) {
                    try { Thread.sleep(200); } catch (InterruptedException ie) { break; }
                }
            }
        }
    }

    private void handle(Socket s) {
        try {
            InputStream in = s.getInputStream();
            OutputStream out = s.getOutputStream();
            String head = readHead(in);
            if (head == null) return;
            lastRequestMs = System.currentTimeMillis();
            String[] lines = head.split("\r\n");
            if (lines.length == 0) { sendSimple(out, 400, "text/plain", "bad request"); return; }
            String[] req = lines[0].split(" ");
            if (req.length < 2) { sendSimple(out, 400, "text/plain", "bad request"); return; }
            String method = req[0];
            String route = req[1];
            String query = "";
            int q = route.indexOf('?');
            if (q >= 0) {
                query = route.substring(q + 1);
                route = route.substring(0, q);
            }
            String contentLength = null;
            for (String l : lines) {
                if (l.regionMatches(true, 0, "Content-Length:", 0, "Content-Length:".length())) {
                    contentLength = l.substring("Content-Length:".length()).trim();
                }
            }
            if ("GET".equals(method)) {
                if ("/v1/status".equals(route)) {
                    sendJson(out, statusJson());
                } else if ("/v1/page".equals(route)) {
                    servePage(out, query);
                } else if ("/v2/apps".equals(route)) {
                    sendJson(out, contentApi.appsJson());
                } else if ("/v2/xhs/feed".equals(route)) {
                    sendJson(out, contentApi.xhsFeedJson());
                } else if ("/v2/xhs/note".equals(route)) {
                    sendJson(out, contentApi.xhsNoteJson());
                } else if ("/v2/xhs/comments".equals(route)) {
                    sendJson(out, contentApi.xhsCommentsJson(intParam(query, "advance", 0) == 1));
                } else if ("/v2/xhs/image".equals(route)) {
                    byte[] bmp = contentApi.xhsImageBmp(intParam(query, "index", 0));
                    if (bmp == null) sendSimple(out, 404, "application/json", "{\"ok\":false,\"error\":\"image unavailable\"}");
                    else sendBytes(out, "image/bmp", bmp);
                } else {
                    sendSimple(out, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
                }
            } else if ("POST".equals(method)) {
                if ("/v1/consume".equals(route)) {
                    drainBody(in, contentLength);
                    int index = intParam(query, "index", -1);
                    if (index >= 0) store.consume(index);
                    if (onConsume != null) onConsume.run();
                    sendJson(out, "{\"ok\":true,\"consumed\":" + index + "}");
                } else if ("/v1/tap".equals(route)) {
                    drainBody(in, contentLength);
                    int x = intParam(query, "x", -1);
                    int y = intParam(query, "y", -1);
                    boolean ok = x >= 0 && y >= 0 && onTap != null
                            && Boolean.TRUE.equals(onTap.apply(x, y));
                    sendJson(out, "{\"ok\":" + ok + "}");
                } else if ("/v2/apps/open".equals(route)) {
                    drainBody(in, contentLength);
                    sendJson(out, "{\"ok\":" + contentApi.openApp(stringParam(query, "id")) + "}");
                } else if ("/v2/xhs/feed/open".equals(route)) {
                    drainBody(in, contentLength);
                    sendJson(out, "{\"ok\":" + contentApi.openXhsNote(stringParam(query, "token")) + "}");
                } else if ("/v2/xhs/comments/open".equals(route)) {
                    drainBody(in, contentLength);
                    sendJson(out, "{\"ok\":" + contentApi.openXhsComments() + "}");
                } else {
                    drainBody(in, contentLength);
                    sendSimple(out, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
                }
            } else {
                sendSimple(out, 405, "text/plain", "method not allowed");
            }
            out.flush();
        } catch (IOException ignored) {
        } finally {
            try { s.close(); } catch (IOException ignored) { }
        }
    }

    private void servePage(OutputStream out, String query) throws IOException {
        int index = intParam(query, "index", -1);
        byte[] raw = index >= 0 ? store.get(index) : null;
        if (raw == null) {
            sendSimple(out, 404, "application/json",
                    "{\"ok\":false,\"error\":\"page not available\",\"index\":" + index + "}");
            return;
        }
        CRC32 crc = new CRC32();
        crc.update(raw);
        byte[] enc = Rle.encode(raw);
        int codec = Rle.codecFor(raw, enc);
        byte[] payload = codec == Rle.CODEC_RLE1 ? enc : raw;
        byte[] header = Header.build(codec, Framebuffer.PHYS_W, Framebuffer.PHYS_H,
                Framebuffer.STRIDE, index, Framebuffer.RAW_SIZE, crc.getValue());
        byte[] head = ("HTTP/1.1 200 OK\r\n"
                + "Content-Type: application/octet-stream\r\n"
                + "Content-Length: " + (header.length + payload.length) + "\r\n"
                + "Connection: close\r\n\r\n").getBytes(StandardCharsets.ISO_8859_1);
        out.write(head);
        out.write(header);
        out.write(payload);
    }

    private String statusJson() {
        return "{\"ok\":true,\"port\":" + PORT
                + ",\"active\":" + ScreenBridgeService.isSessionActive()
                + ",\"cacheEnabled\":" + (cacheEnabled == null || cacheEnabled.getAsBoolean())
                + ",\"captureErrorCode\":" + ScreenBridgeService.captureErrorCodeSnapshot()
                + ",\"pages\":{\"lo\":" + store.lo() + ",\"hi\":" + store.hi()
                + ",\"count\":" + store.count() + "}"
                + ",\"consumed\":" + store.consumedIndex()
                + ",\"connected\":" + connected() + "}";
    }

    private static String readHead(InputStream in) throws IOException {
        byte[] buf = new byte[8192];
        int n = 0;
        while (n < buf.length) {
            int b = in.read();
            if (b < 0) break;
            buf[n++] = (byte) b;
            if (n >= 4 && buf[n - 4] == '\r' && buf[n - 3] == '\n'
                    && buf[n - 2] == '\r' && buf[n - 1] == '\n') {
                return new String(buf, 0, n, StandardCharsets.ISO_8859_1);
            }
        }
        return n == 0 ? null : new String(buf, 0, n, StandardCharsets.ISO_8859_1);
    }

    private static void drainBody(InputStream in, String contentLength) {
        int len = 0;
        if (contentLength != null) {
            try { len = Integer.parseInt(contentLength); } catch (NumberFormatException ignored) { }
        }
        while (len-- > 0) {
            try { if (in.read() < 0) break; } catch (IOException e) { break; }
        }
    }

    private static int intParam(String query, String name, int def) {
        int i = query.indexOf(name + "=");
        if (i < 0) return def;
        int v = 0;
        boolean any = false;
        for (int j = i + name.length() + 1; j < query.length(); j++) {
            char c = query.charAt(j);
            if (c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
            any = true;
        }
        return any ? v : def;
    }

    private static String stringParam(String query, String name) {
        String prefix = name + "=";
        for (String part : query.split("&")) {
            if (part.startsWith(prefix)) return part.substring(prefix.length());
        }
        return "";
    }

    private static void sendJson(OutputStream out, String body) throws IOException {
        sendSimple(out, 200, "application/json", body);
    }

    private static void sendBytes(OutputStream out, String type, byte[] body) throws IOException {
        out.write(("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: "
                + body.length + "\r\nConnection: close\r\n\r\n")
                .getBytes(StandardCharsets.ISO_8859_1));
        out.write(body);
    }

    private static void sendSimple(OutputStream out, int status, String type, String body)
            throws IOException {
        byte[] b = body.getBytes(StandardCharsets.UTF_8);
        String reason = status == 200 ? "OK"
                : status == 404 ? "Not Found"
                : status == 400 ? "Bad Request"
                : status == 405 ? "Method Not Allowed" : "Error";
        out.write(("HTTP/1.1 " + status + " " + reason + "\r\n"
                + "Content-Type: " + type + (type.startsWith("text/") || type.contains("json")
                        ? "; charset=utf-8" : "") + "\r\n"
                + "Content-Length: " + b.length + "\r\n"
                + "Connection: close\r\n\r\n").getBytes(StandardCharsets.ISO_8859_1));
        out.write(b);
    }
}
