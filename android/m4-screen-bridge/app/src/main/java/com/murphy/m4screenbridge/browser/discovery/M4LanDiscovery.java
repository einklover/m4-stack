package com.murphy.m4screenbridge.browser.discovery;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Pure-Java Browser Bridge LAN discovery contract. Android NsdManager feeds
 * resolved records in; tests drive the same engine without the framework.
 *
 * Precedence: manual override &gt; discovered &gt; cached last-known &gt; none.
 * Explicit {@code loopback}/{@code local} bypasses discovery and stays local.
 */
public final class M4LanDiscovery {
    public static final String SERVICE = "m4b3";
    public static final String PROTO = "tcp";
    public static final String SERVICE_TYPE = "_m4b3._tcp";
    public static final String SERVICE_TYPE_DOT = "_m4b3._tcp.";
    public static final String INSTANCE_NAME = "murphy-m4-browser";
    public static final String HOSTNAME = "murphy-m4";
    public static final String TXT_PROTO_KEY = "proto";
    public static final String TXT_PROTO_VAL = "m4b3";
    public static final String TXT_ROLE_KEY = "role";
    public static final String TXT_ROLE_VAL = "browser-bridge";
    public static final int DEFAULT_PORT = 48624;
    public static final int MAX_CANDIDATES = 8;
    public static final int MAX_DISCOVER_RESTARTS = 3;
    public static final long SEARCH_TIMEOUT_MS = 8000L;

    public enum Source { MANUAL, DISCOVERED, CACHED, NONE, LOOPBACK }

    public enum Phase { IDLE, MANUAL, LOOPBACK, SEARCHING, SELECTED, LOST, STOPPED }

    public enum HostMode { AUTO, MANUAL, LOOPBACK }

    public static final class Endpoint {
        public final String name;
        public final String host;
        public final int port;

        public Endpoint(String name, String host, int port) {
            this.name = name == null ? "" : name;
            this.host = host == null ? "" : host;
            this.port = port;
        }

        public String key() {
            return host.toLowerCase(Locale.ROOT) + ":" + port;
        }

        public String display() {
            return host.isEmpty() ? "" : host + ":" + port;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) return true;
            if (!(o instanceof Endpoint)) return false;
            Endpoint e = (Endpoint) o;
            return port == e.port && host.equalsIgnoreCase(e.host);
        }

        @Override
        public int hashCode() {
            return key().hashCode();
        }
    }

    public static final class Decision {
        public final Source source;
        public final Phase phase;
        public final Endpoint endpoint;
        public final String reason;

        public Decision(Source source, Phase phase, Endpoint endpoint, String reason) {
            this.source = source;
            this.phase = phase;
            this.endpoint = endpoint;
            this.reason = reason == null ? "" : reason;
        }

        public boolean hasEndpoint() {
            return endpoint != null && validHost(endpoint.host) && validPort(endpoint.port);
        }
    }

    public static HostMode classify(String raw) {
        if (raw == null) return HostMode.AUTO;
        String h = raw.trim();
        if (h.isEmpty()) return HostMode.AUTO;
        if ("loopback".equalsIgnoreCase(h) || "local".equalsIgnoreCase(h)) return HostMode.LOOPBACK;
        return HostMode.MANUAL;
    }

    public static String normalizeType(String raw) {
        if (raw == null) return "";
        String t = raw.trim().toLowerCase(Locale.ROOT);
        if (t.endsWith(".local")) t = t.substring(0, t.length() - 6);
        while (t.startsWith(".")) t = t.substring(1);
        while (t.endsWith(".")) t = t.substring(0, t.length() - 1);
        if (t.contains("_m4b3._tcp")) return SERVICE_TYPE;
        return t;
    }

    public static boolean validPort(int port) {
        return port >= 1 && port <= 65535;
    }

    public static boolean validHost(String raw) {
        if (raw == null) return false;
        String h = raw.trim();
        if (h.isEmpty() || h.length() > 253) return false;
        if ("0.0.0.0".equals(h) || "255.255.255.255".equals(h)) return false;
        if ("loopback".equalsIgnoreCase(h) || "local".equalsIgnoreCase(h)) return false;
        if (h.contains("://") || h.indexOf(' ') >= 0 || h.indexOf('\t') >= 0) return false;
        if (h.indexOf('/') >= 0 || h.indexOf('\\') >= 0 || h.indexOf(':') >= 0 || h.indexOf('@') >= 0) {
            return false;
        }
        if (h.charAt(0) == '.' || h.charAt(h.length() - 1) == '.') return false;
        for (int i = 0; i < h.length(); i++) {
            char c = h.charAt(i);
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    public static boolean protoTxtOk(String protoTxt) {
        if (protoTxt == null) return true;
        String p = protoTxt.trim();
        return p.isEmpty() || TXT_PROTO_VAL.equalsIgnoreCase(p);
    }

    public static Endpoint parseRecord(String name, String type, String host, int port, String protoTxt) {
        if (!SERVICE_TYPE.equals(normalizeType(type))) return null;
        if (!validHost(host) || !validPort(port) || !protoTxtOk(protoTxt)) return null;
        String n = name == null ? "" : name.trim();
        if (n.isEmpty()) n = INSTANCE_NAME;
        if (n.length() > 63) return null;
        return new Endpoint(n, host.trim(), port);
    }

    public static List<Endpoint> dedup(List<Endpoint> in) {
        if (in == null || in.isEmpty()) return Collections.emptyList();
        Map<String, Endpoint> byKey = new LinkedHashMap<String, Endpoint>();
        for (int i = 0; i < in.size(); i++) {
            Endpoint e = in.get(i);
            if (e == null || !validHost(e.host) || !validPort(e.port)) continue;
            Endpoint prev = byKey.get(e.key());
            if (prev == null || e.name.compareTo(prev.name) < 0) byKey.put(e.key(), e);
        }
        return new ArrayList<Endpoint>(byKey.values());
    }

    public static Endpoint select(List<Endpoint> in) {
        List<Endpoint> unique = dedup(in);
        if (unique.isEmpty()) return null;
        Collections.sort(unique, new Comparator<Endpoint>() {
            @Override
            public int compare(Endpoint a, Endpoint b) {
                int h = a.host.toLowerCase(Locale.ROOT).compareTo(b.host.toLowerCase(Locale.ROOT));
                if (h != 0) return h;
                if (a.port != b.port) return a.port - b.port;
                return a.name.compareTo(b.name);
            }
        });
        return unique.get(0);
    }

    public static Decision resolve(String manualRaw, int manualPort, List<Endpoint> live, Endpoint cached) {
        HostMode mode = classify(manualRaw);
        if (mode == HostMode.LOOPBACK) {
            return new Decision(Source.LOOPBACK, Phase.LOOPBACK, null, "explicit-loopback");
        }
        if (mode == HostMode.MANUAL) {
            String host = manualRaw == null ? "" : manualRaw.trim();
            if (validHost(host) && validPort(manualPort)) {
                return new Decision(Source.MANUAL, Phase.MANUAL, new Endpoint("manual", host, manualPort),
                        "manual-override");
            }
            return new Decision(Source.NONE, Phase.IDLE, null, "manual-invalid");
        }
        Endpoint chosen = select(live);
        if (chosen != null) {
            return new Decision(Source.DISCOVERED, Phase.SELECTED, chosen, "live");
        }
        if (cached != null && validHost(cached.host) && validPort(cached.port)) {
            return new Decision(Source.CACHED, Phase.SELECTED, cached, "cached");
        }
        return new Decision(Source.NONE, Phase.SEARCHING, null, "no-host");
    }

    /**
     * Lifecycle-bounded selector. One live set, one selected endpoint, capped
     * discover restarts. {@link #stop()} ignores further events.
     */
    public static final class Engine {
        private final Object lock = new Object();
        private Phase phase = Phase.IDLE;
        private Source source = Source.NONE;
        private final LinkedHashMap<String, Endpoint> live = new LinkedHashMap<String, Endpoint>();
        private Endpoint cached;
        private Endpoint selected;
        private String lastError = "";
        private String reason = "";
        private int foundCount;
        private int lostCount;
        private int rejectedCount;
        private int retryCount;
        private int discoverRestarts;
        private boolean running;
        private boolean stopped;
        private boolean endpointChanged;
        private boolean wantDiscoverRestart;
        private long searchStartedMs;
        private long lastEventMs;

        public void setManual(String host, int port) {
            synchronized (lock) {
                stopLocked();
                Decision d = resolve(host, port, null, null);
                applyDecisionLocked(d);
            }
        }

        public void setLoopback() {
            synchronized (lock) {
                stopLocked();
                applyDecisionLocked(new Decision(Source.LOOPBACK, Phase.LOOPBACK, null, "explicit-loopback"));
            }
        }

        public void startAuto(Endpoint cachedEndpoint, long nowMs) {
            synchronized (lock) {
                if (stopped) return;
                running = true;
                stopped = false;
                phase = Phase.SEARCHING;
                source = Source.NONE;
                cached = cachedEndpoint;
                searchStartedMs = nowMs;
                lastEventMs = nowMs;
                reason = "searching";
                applyDecisionLocked(resolve("", DEFAULT_PORT, liveListLocked(), cached));
            }
        }

        public void onResolved(Endpoint record, long nowMs) {
            synchronized (lock) {
                if (!acceptEventLocked()) return;
                lastEventMs = nowMs;
                if (record == null) {
                    rejectedCount++;
                    lastError = "invalid-record";
                    return;
                }
                if (live.size() >= MAX_CANDIDATES && !live.containsKey(record.key())) {
                    rejectedCount++;
                    lastError = "candidate-cap";
                    return;
                }
                Endpoint prev = live.put(record.key(), record);
                if (prev == null) foundCount++;
                applyDecisionLocked(resolve("", DEFAULT_PORT, liveListLocked(), cached));
            }
        }

        public void onLost(String host, int port, long nowMs) {
            synchronized (lock) {
                if (!acceptEventLocked()) return;
                lastEventMs = nowMs;
                Endpoint removed = null;
                if (validHost(host) && validPort(port)) {
                    removed = live.remove(new Endpoint("", host, port).key());
                }
                if (removed != null) lostCount++;
                if (cached != null && host != null && cached.host.equalsIgnoreCase(host.trim())
                        && cached.port == port) {
                    cached = null;
                }
                Decision d = resolve("", DEFAULT_PORT, liveListLocked(), cached);
                if (!d.hasEndpoint() && running) {
                    phase = live.isEmpty() ? Phase.LOST : Phase.SEARCHING;
                    source = Source.NONE;
                    selected = null;
                    reason = "lost";
                    endpointChanged = true;
                    searchStartedMs = nowMs;
                } else {
                    applyDecisionLocked(d);
                }
            }
        }

        public void onError(String message, long nowMs) {
            synchronized (lock) {
                if (!acceptEventLocked()) return;
                lastEventMs = nowMs;
                lastError = message == null ? "error" : message;
            }
        }

        public void tick(long nowMs) {
            synchronized (lock) {
                if (!acceptEventLocked()) return;
                if (phase != Phase.SEARCHING && phase != Phase.LOST) return;
                if (nowMs - searchStartedMs < SEARCH_TIMEOUT_MS) return;
                if (discoverRestarts >= MAX_DISCOVER_RESTARTS) return;
                discoverRestarts++;
                retryCount++;
                wantDiscoverRestart = true;
                searchStartedMs = nowMs;
                reason = "retry";
            }
        }

        public boolean consumeDiscoverRestart() {
            synchronized (lock) {
                boolean v = wantDiscoverRestart;
                wantDiscoverRestart = false;
                return v;
            }
        }

        public boolean consumeEndpointChanged() {
            synchronized (lock) {
                boolean v = endpointChanged;
                endpointChanged = false;
                return v;
            }
        }

        public void stop() {
            synchronized (lock) {
                stopLocked();
            }
        }

        public boolean isStopped() {
            synchronized (lock) {
                return stopped;
            }
        }

        public Decision decision() {
            synchronized (lock) {
                return new Decision(source, phase, selected, reason);
            }
        }

        public String snapshot() {
            synchronized (lock) {
                String ep = selected == null ? "-" : selected.display();
                return "discovery src=" + source
                        + " phase=" + phase
                        + " ep=" + ep
                        + " live=" + live.size()
                        + " retries=" + retryCount
                        + " lost=" + lostCount
                        + " rejected=" + rejectedCount
                        + " restarts=" + discoverRestarts
                        + (lastError.isEmpty() ? "" : " err=" + lastError)
                        + (reason.isEmpty() ? "" : " why=" + reason);
            }
        }

        public int liveCount() {
            synchronized (lock) {
                return live.size();
            }
        }

        public int retryCount() {
            synchronized (lock) {
                return retryCount;
            }
        }

        public String lastError() {
            synchronized (lock) {
                return lastError;
            }
        }

        private boolean acceptEventLocked() {
            return running && !stopped && phase != Phase.MANUAL && phase != Phase.LOOPBACK;
        }

        private void stopLocked() {
            running = false;
            stopped = true;
            phase = Phase.STOPPED;
            live.clear();
            wantDiscoverRestart = false;
        }

        private List<Endpoint> liveListLocked() {
            return new ArrayList<Endpoint>(live.values());
        }

        private void applyDecisionLocked(Decision d) {
            boolean changed = source != d.source
                    || phase != d.phase
                    || !sameEndpoint(selected, d.endpoint);
            source = d.source;
            phase = d.phase;
            selected = d.endpoint;
            reason = d.reason;
            if (changed) endpointChanged = true;
        }

        private static boolean sameEndpoint(Endpoint a, Endpoint b) {
            if (a == null && b == null) return true;
            if (a == null || b == null) return false;
            return a.equals(b);
        }
    }

    private M4LanDiscovery() {}
}
