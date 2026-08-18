package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.Decision;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.Endpoint;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.Engine;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.HostMode;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.Phase;
import com.murphy.m4screenbridge.browser.discovery.M4LanDiscovery.Source;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Pure-Java LAN discovery parse/select/lifecycle checks. */
public final class M4LanDiscoveryTest {
    public static void main(String[] args) {
        parseAndValidate();
        duplicateCandidates();
        selectionIsDeterministic();
        precedenceManualDiscoveredCachedNone();
        manualOverrideBypassesDiscovery();
        invalidAndEmptyHostPort();
        discoveryLossAndRetry();
        lifecycleCleanup();
        cachedFallback();
        stressDedupAndLoss();
        System.out.println("OK: M4 LAN discovery self-checks passed");
    }

    private static void parseAndValidate() {
        Endpoint a = M4LanDiscovery.parseRecord("murphy-m4-browser", "_m4b3._tcp.",
                "192.168.0.152", 48624, "m4b3");
        assertTrue(a != null, "valid record");
        assertEquals("192.168.0.152", a.host, "host");
        assertEquals(48624, a.port, "port");
        assertTrue(M4LanDiscovery.parseRecord("n", "_M4B3._TCP", "murphy-m4.local", 48624, "") != null,
                "hostname + missing TXT");
        assertTrue(M4LanDiscovery.parseRecord("n", "._m4b3._tcp.local.", "10.0.0.8", 48624, null) != null,
                "android type variant");
        assertTrue(M4LanDiscovery.parseRecord("n", "_http._tcp.", "192.168.0.152", 48624, "m4b3") == null,
                "wrong type");
        assertTrue(M4LanDiscovery.parseRecord("n", "_m4b3._tcp.", "0.0.0.0", 48624, "m4b3") == null,
                "unspecified ip");
        assertTrue(M4LanDiscovery.parseRecord("n", "_m4b3._tcp.", "fe80::1", 48624, "m4b3") == null,
                "ipv6 rejected");
        assertTrue(M4LanDiscovery.parseRecord("n", "_m4b3._tcp.", "192.168.0.152", 0, "m4b3") == null,
                "port 0");
        assertTrue(M4LanDiscovery.parseRecord("n", "_m4b3._tcp.", "", 48624, "m4b3") == null, "empty host");
        assertTrue(M4LanDiscovery.parseRecord("n", "_m4b3._tcp.", "192.168.0.152", 48624, "http") == null,
                "wrong proto txt");
        assertEquals("_m4b3._tcp", M4LanDiscovery.SERVICE_TYPE, "service type");
        assertEquals(48624, M4LanDiscovery.DEFAULT_PORT, "default port");
    }

    private static void duplicateCandidates() {
        List<Endpoint> in = Arrays.asList(
                rec("b", "192.168.0.10", 48624),
                rec("a", "192.168.0.10", 48624),
                rec("c", "192.168.0.10", 48624));
        List<Endpoint> out = M4LanDiscovery.dedup(in);
        assertEquals(1, out.size(), "dedup size");
        assertEquals("a", out.get(0).name, "stable name wins");
        Endpoint chosen = M4LanDiscovery.select(in);
        assertTrue(chosen != null, "select dup");
        assertEquals("192.168.0.10", chosen.host, "dup host");
        assertEquals("a", chosen.name, "dup name");
    }

    private static void selectionIsDeterministic() {
        List<Endpoint> in = Arrays.asList(
                rec("z", "192.168.0.20", 48624),
                rec("m", "10.0.0.8", 48625),
                rec("a", "10.0.0.8", 48624));
        Endpoint first = M4LanDiscovery.select(in);
        Endpoint second = M4LanDiscovery.select(new ArrayList<Endpoint>(in));
        assertTrue(first != null && second != null, "select both");
        assertEquals(first.key(), second.key(), "stable key");
        assertEquals("10.0.0.8", first.host, "lowest host");
        assertEquals(48624, first.port, "then lowest port");
        for (int i = 0; i < 64; i++) {
            assertEquals("10.0.0.8:48624", M4LanDiscovery.select(in).key(), "repeat " + i);
        }
    }

    private static void precedenceManualDiscoveredCachedNone() {
        Endpoint live = rec("live", "192.168.0.152", 48624);
        Endpoint cached = rec("cached", "192.168.0.10", 48624);
        Decision manual = M4LanDiscovery.resolve("192.168.0.99", 48624, Arrays.asList(live), cached);
        assertEquals(Source.MANUAL, manual.source, "manual wins");
        assertEquals("192.168.0.99", manual.endpoint.host, "manual host");
        Decision discovered = M4LanDiscovery.resolve("", 48624, Arrays.asList(live), cached);
        assertEquals(Source.DISCOVERED, discovered.source, "live over cache");
        Decision cacheOnly = M4LanDiscovery.resolve("", 48624, Arrays.asList(), cached);
        assertEquals(Source.CACHED, cacheOnly.source, "cache when no live");
        Decision none = M4LanDiscovery.resolve("", 48624, Arrays.asList(), null);
        assertEquals(Source.NONE, none.source, "no-host");
        Decision loop = M4LanDiscovery.resolve("loopback", 48624, Arrays.asList(live), cached);
        assertEquals(Source.LOOPBACK, loop.source, "loopback");
        assertEquals(HostMode.AUTO, M4LanDiscovery.classify(""), "empty auto");
        assertEquals(HostMode.AUTO, M4LanDiscovery.classify("  "), "blank auto");
        assertEquals(HostMode.LOOPBACK, M4LanDiscovery.classify("LOCAL"), "LOCAL loopback");
        assertEquals(HostMode.MANUAL, M4LanDiscovery.classify("192.168.0.1"), "ip manual");
    }

    private static void manualOverrideBypassesDiscovery() {
        Engine e = new Engine();
        e.setManual("192.168.0.77", 48624);
        Decision d = e.decision();
        assertEquals(Source.MANUAL, d.source, "manual source");
        e.onResolved(rec("x", "10.0.0.1", 48624), 100);
        assertEquals(Source.MANUAL, e.decision().source, "resolved ignored");
        assertEquals(0, e.liveCount(), "no live in manual");
        e.setManual("not a host", 48624);
        assertEquals(Source.NONE, e.decision().source, "invalid manual");
    }

    private static void invalidAndEmptyHostPort() {
        assertTrue(!M4LanDiscovery.validHost(""), "empty");
        assertTrue(!M4LanDiscovery.validHost("   "), "blank");
        assertTrue(!M4LanDiscovery.validHost("loopback"), "loopback not a lan host");
        assertTrue(!M4LanDiscovery.validPort(0), "port 0");
        assertTrue(!M4LanDiscovery.validPort(65536), "port high");
        Decision bad = M4LanDiscovery.resolve("http://x", 48624, null, null);
        assertEquals(Source.NONE, bad.source, "url rejected");
        Decision badPort = M4LanDiscovery.resolve("192.168.0.1", 0, null, null);
        assertEquals(Source.NONE, badPort.source, "bad port");
    }

    private static void discoveryLossAndRetry() {
        Engine e = new Engine();
        e.startAuto(null, 0);
        assertEquals(Phase.SEARCHING, e.decision().phase, "searching");
        e.onResolved(rec("a", "192.168.0.152", 48624), 10);
        assertEquals(Source.DISCOVERED, e.decision().source, "found");
        e.onLost("192.168.0.152", 48624, 20);
        assertEquals(Source.NONE, e.decision().source, "lost to none");
        assertTrue(e.decision().phase == Phase.LOST || e.decision().phase == Phase.SEARCHING, "lost phase");
        e.tick(20 + M4LanDiscovery.SEARCH_TIMEOUT_MS);
        assertTrue(e.consumeDiscoverRestart(), "first restart");
        e.tick(20 + 2 * M4LanDiscovery.SEARCH_TIMEOUT_MS);
        e.tick(20 + 3 * M4LanDiscovery.SEARCH_TIMEOUT_MS);
        e.tick(20 + 4 * M4LanDiscovery.SEARCH_TIMEOUT_MS);
        assertTrue(e.retryCount() <= M4LanDiscovery.MAX_DISCOVER_RESTARTS, "retry cap");
        assertTrue(!e.consumeDiscoverRestart() || e.retryCount() == M4LanDiscovery.MAX_DISCOVER_RESTARTS,
                "no unbounded restart");
        e.onResolved(rec("a", "192.168.0.152", 48624), 100000);
        assertEquals(Source.DISCOVERED, e.decision().source, "recovered");
    }

    private static void lifecycleCleanup() {
        Engine e = new Engine();
        e.startAuto(null, 0);
        e.onResolved(rec("a", "10.0.0.2", 48624), 1);
        e.stop();
        assertTrue(e.isStopped(), "stopped");
        assertEquals(Phase.STOPPED, e.decision().phase, "phase stopped");
        e.onResolved(rec("b", "10.0.0.3", 48624), 2);
        assertEquals(0, e.liveCount(), "events ignored after stop");
        e.tick(M4LanDiscovery.SEARCH_TIMEOUT_MS * 5);
        assertTrue(!e.consumeDiscoverRestart(), "no restart after stop");
    }

    private static void cachedFallback() {
        Endpoint cached = rec("cached", "192.168.0.88", 48624);
        Engine e = new Engine();
        e.startAuto(cached, 0);
        Decision d = e.decision();
        assertEquals(Source.CACHED, d.source, "cache while searching");
        assertEquals("192.168.0.88", d.endpoint.host, "cache host");
        e.onResolved(rec("live", "192.168.0.152", 48624), 5);
        assertEquals(Source.DISCOVERED, e.decision().source, "live replaces cache");
        e.onLost("192.168.0.152", 48624, 6);
        assertEquals(Source.CACHED, e.decision().source, "back to cache");
        e.onLost("192.168.0.88", 48624, 7);
        assertEquals(Source.NONE, e.decision().source, "cache invalidated on loss");
    }

    private static void stressDedupAndLoss() {
        Engine e = new Engine();
        e.startAuto(null, 0);
        for (int i = 0; i < 64; i++) {
            e.onResolved(rec("n" + (i % 3), "192.168.0.5", 48624), i);
        }
        assertEquals(1, e.liveCount(), "64 identical collapse");
        assertEquals(Source.DISCOVERED, e.decision().source, "still selected");
        for (int i = 0; i < 20; i++) {
            e.onResolved(rec("x" + i, "10.0.0." + (i + 1), 48624), 100 + i);
        }
        assertTrue(e.liveCount() <= M4LanDiscovery.MAX_CANDIDATES, "cap");
        for (int i = 1; i <= 8; i++) {
            e.onLost("10.0.0." + i, 48624, 200 + i);
        }
        e.onLost("192.168.0.5", 48624, 300);
        assertEquals(Source.NONE, e.decision().source, "all gone");
    }

    private static Endpoint rec(String name, String host, int port) {
        Endpoint e = M4LanDiscovery.parseRecord(name, M4LanDiscovery.SERVICE_TYPE_DOT, host, port, "m4b3");
        assertTrue(e != null, "fixture " + host);
        return e;
    }

    private static void assertTrue(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    private static void assertEquals(Object expected, Object actual, String msg) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(msg + ": expected " + expected + " got " + actual);
        }
    }
}
