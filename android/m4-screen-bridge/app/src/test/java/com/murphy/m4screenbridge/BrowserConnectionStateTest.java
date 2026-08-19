package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.session.BrowserConnectionState;

public final class BrowserConnectionStateTest {
    public static void main(String[] args) {
        testAutoDiscoveryAndReconnect();
        testReconnectAttemptStaysReconnectingUntilProtocolReady();
        testSameEndpointPromotionPreservesConnection();
        testDifferentDiscoveredEndpointReconnects();
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
        s.discoveryError("nsd transient");
        eq(BrowserConnectionState.State.DISCOVERING, s.snapshot().state);
        eq("nsd transient", s.snapshot().error);
        s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED, "192.168.0.152", 48624);
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
        s.disconnected("eof", true);
        eq(BrowserConnectionState.State.RECONNECTING, s.snapshot().state);
        eq(1L, s.snapshot().reconnects);
        // Error + disconnected for the same incident must not double count.
        s.disconnected("SocketException", true);
        eq(1L, s.snapshot().reconnects);
        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
        s.clearAutoEndpoint("service lost");
        eq(BrowserConnectionState.State.DISCOVERING, s.snapshot().state);
        eq(BrowserConnectionState.Source.NONE, s.snapshot().source);
        s.stop();
        eq(BrowserConnectionState.State.DISABLED, s.snapshot().state);
    }

    private static void testReconnectAttemptStaysReconnectingUntilProtocolReady() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startAuto();
        s.selectAutoEndpoint(BrowserConnectionState.Source.DISCOVERED, "10.0.0.2", 48624);
        s.connected();
        s.disconnected("eof", true);
        eq(BrowserConnectionState.State.RECONNECTING, s.snapshot().state);

        // A replacement TCP socket is only another transport attempt. Product CONNECTED must
        // remain gated on the M4B3 HELLO handshake, so beginning that attempt must not erase
        // the reconnecting state or hide the failure strip.
        s.connecting();
        eq(BrowserConnectionState.State.RECONNECTING, s.snapshot().state);

        // Protocol readiness is the event that clears the reconnect state.
        s.connected();
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
    }

    private static void testSameEndpointPromotionPreservesConnection() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startAuto();
        s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED, "10.0.0.2", 48624);
        s.connected();
        s.selectAutoEndpoint(BrowserConnectionState.Source.DISCOVERED, "10.0.0.2", 48624);
        eq(BrowserConnectionState.State.CONNECTED, s.snapshot().state);
        eq(BrowserConnectionState.Source.DISCOVERED, s.snapshot().source);
        eq("10.0.0.2:48624", s.snapshot().endpoint());
    }

    private static void testDifferentDiscoveredEndpointReconnects() {
        BrowserConnectionState s = new BrowserConnectionState();
        s.startAuto();
        s.selectAutoEndpoint(BrowserConnectionState.Source.CACHED, "10.0.0.2", 48624);
        s.connected();
        s.selectAutoEndpoint(BrowserConnectionState.Source.DISCOVERED, "10.0.0.3", 48624);
        eq(BrowserConnectionState.State.CONNECTING, s.snapshot().state);
        eq(BrowserConnectionState.Source.DISCOVERED, s.snapshot().source);
        eq("10.0.0.3:48624", s.snapshot().endpoint());
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
        eq(BrowserConnectionState.State.DISABLED, s.snapshot().state);
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
