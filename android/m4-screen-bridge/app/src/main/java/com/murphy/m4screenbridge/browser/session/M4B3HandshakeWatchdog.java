package com.murphy.m4screenbridge.browser.session;

/**
 * Tracks the boundary between a connected TCP socket and a usable M4B3 session.
 *
 * <p>A TCP connect is not product readiness: the peer must answer the M4B3 HELLO. If the current
 * transport generation stays silent past the deadline, the owner should tear down that transport
 * and create a fresh session. Stale callbacks from previous transports cannot trip the watchdog.</p>
 */
public final class M4B3HandshakeWatchdog {
    private final long timeoutMs;
    private long generation = -1;
    private long socketConnectedAtMs = -1;

    public M4B3HandshakeWatchdog(long timeoutMs) {
        if (timeoutMs <= 0) throw new IllegalArgumentException("timeoutMs must be positive");
        this.timeoutMs = timeoutMs;
    }

    public synchronized void onSocketConnected(long newGeneration, long nowMs) {
        generation = newGeneration;
        socketConnectedAtMs = nowMs;
    }

    public synchronized void onProtocolReady(long readyGeneration) {
        if (readyGeneration != generation) return;
        socketConnectedAtMs = -1;
    }

    public synchronized void onTransportLost(long lostGeneration) {
        if (lostGeneration != generation) return;
        socketConnectedAtMs = -1;
    }

    public synchronized void reset() {
        generation = -1;
        socketConnectedAtMs = -1;
    }

    public synchronized boolean shouldRestart(long currentGeneration, long nowMs) {
        if (currentGeneration != generation || socketConnectedAtMs < 0) return false;
        return nowMs - socketConnectedAtMs >= timeoutMs;
    }
}
