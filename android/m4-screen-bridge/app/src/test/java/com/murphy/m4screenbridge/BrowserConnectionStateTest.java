package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.session.BrowserConnectionState;

public final class BrowserConnectionStateTest {
    public static void main(String[] args) {
        testAutoDiscoveryAndReconnect();
        testDiscoveredOverridesCached();
        testManualFailure();
        testLoopback();
        testValidation();
        System.out.println("BrowserConnectionStateTest PASS");
    }

    private static void testAutoDiscoveryAndReconnect() {
        BrowserConnectionState s = new BrowserConnectionState();
        eq(BrowserConnectionState.State.DISABLED, s.snapshot().state);

        s.startAuto();
        eq(BrowserConnectionState.State.DISCOVERING, s.snapshot().state);
        eq(BrowserConnectionState.Mode.AUTO, s.snapshot().mode);
        eq(BrowserConnectionState.Source.NONE, s.snapshot().source);

        s.discoveryError("nsd transient");
        eq(BrowserConnectionState.State.DISCOVERING, s.snapshot().state);
        eq("nsd transient", s.snapshot().error);

        s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED, "192.168.0.152", 48624);
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        eq(BrowserConnectionState.Source.CACHED, s.snapshot().source);
        eq("192.168.0.152:48624", s.snapshot().endpoint());

        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
        eq("", s.snapshot().error);

        s.disconnected("eof", true);
        eq(BrowserConnectionState.State.RECONNECTING, s.snapshot().state);
        eq(1L, s.snapshot().reconnects);
        eq("eof", s.snapshot().error);

        s.connecting();
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);

        s.clearAutoEndpoint("service lost");
        eq(BrowserConnectionState.State.DISCOVERING, s.snapshot().state);
        eq(BrowserConnectionState.Source.NONE, s.snapshot().source);
        eq("", s.snapshot().endpoint());

        s.stop();
        eq(BrowserConnectionState.State.DISABLED, s.snapshot().state);
        eq(BrowserConnectionState.Mode.NONE, s.snapshot().mode);
    }

    private static void testDiscoveredOverridesCached() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startAuto();
        s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED, "10.0.0.2", 48624);
        s.connected();
        s.selectAutoEndpoint(BrowserConnectionState.Source.DISCOVERED, "10.0.0.3", 48624);
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        eq(BrowserConnectionState.Source.DISCOVERED, s.snapshot().source);
        eq("10.0.0.3:48624", s.snapshot().endpoint());
        eq(0L, s.snapshot().reconnects);
    }

    private static void testManualFailure() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startManual("192.168.50.5", 1234);
        eq(BrowserConnectionState.Mode.MANUAL, s.snapshot().mode);
        eq(BrowserConnectionState.Source.MANUAL, s.snapshot().source);
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        s.disconnected("refused", false);
        eq(BrowserConnectionState.State.ERROR, s.snapshot().state);
        eq("refused", s.snapshot().error);
    }

    private static void testLoopback() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startLoopback();
        eq(BrowserConnectionState.Source.LOOPBACK, s.snapshot().source);
        eq("loopback", s.snapshot().endpoint());
        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
        s.disconnected("loopback stopped", true);
        eq(BrowserConnectionState.State.RECONNECTING, s.snapshot().state);
    }

    private static void testValidation() {
        BrowserConnectionState s = new BrowserConnectionState();
        expectThrows(() -> s.startManual("", 48624));
        s.startAuto();
        expectThrows(() -> s.selectAutoEndpoint(BrowserConnectionState.Source.MANUAL,
                "192.168.0.2", 48624));
        expectThrows(() -> s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED,
                "192.168.0.2", 0));
    }

    private static void expectThrows(Runnable action) {
        boolean threw = false;
        try {
            action.run();
        } catch (RuntimeException expected) {
            threw = true;
        }
        if (!threw) throw new AssertionError("expected RuntimeException");
    }

    private static void eq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected=" + expected + " actual=" + actual);
        }
    }

    private static void eq(long expected, long actual) {
        if (expected != actual) {
            throw new AssertionError("expected=" + expected + " actual=" + actual);
        }
    }
}
