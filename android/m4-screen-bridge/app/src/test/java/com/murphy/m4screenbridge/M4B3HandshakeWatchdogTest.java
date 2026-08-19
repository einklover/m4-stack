package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.session.M4B3HandshakeWatchdog;

/** Pure-Java checks for the TCP-connected / M4B3-ready boundary. */
public final class M4B3HandshakeWatchdogTest {
    public static void main(String[] args) {
        timeoutRequiresCurrentUnreadySocket();
        protocolReadyCancelsDeadline();
        transportLossCancelsDeadline();
        newGenerationRearmsDeadline();
        System.out.println("M4B3HandshakeWatchdogTest PASS");
    }

    private static void timeoutRequiresCurrentUnreadySocket() {
        M4B3HandshakeWatchdog w = new M4B3HandshakeWatchdog(3000);
        no(w.shouldRestart(7, 999), "unarmed watchdog");
        w.onSocketConnected(7, 1000);
        no(w.shouldRestart(7, 3999), "before deadline");
        yes(w.shouldRestart(7, 4000), "deadline expires");
        no(w.shouldRestart(6, 9999), "stale transport generation");
    }

    private static void protocolReadyCancelsDeadline() {
        M4B3HandshakeWatchdog w = new M4B3HandshakeWatchdog(3000);
        w.onSocketConnected(11, 2000);
        w.onProtocolReady(11);
        no(w.shouldRestart(11, 9000), "HELLO_OK cancels restart");
    }

    private static void transportLossCancelsDeadline() {
        M4B3HandshakeWatchdog w = new M4B3HandshakeWatchdog(3000);
        w.onSocketConnected(21, 5000);
        w.onTransportLost(21);
        no(w.shouldRestart(21, 9000), "known disconnect cancels stale deadline");
    }

    private static void newGenerationRearmsDeadline() {
        M4B3HandshakeWatchdog w = new M4B3HandshakeWatchdog(3000);
        w.onSocketConnected(31, 1000);
        w.onSocketConnected(32, 3500);
        no(w.shouldRestart(32, 6499), "new socket gets fresh deadline");
        yes(w.shouldRestart(32, 6500), "new socket eventually expires");
        no(w.shouldRestart(31, 6500), "old generation cannot trip new socket");
    }

    private static void yes(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void no(boolean value, String message) {
        if (value) throw new AssertionError(message);
    }
}
