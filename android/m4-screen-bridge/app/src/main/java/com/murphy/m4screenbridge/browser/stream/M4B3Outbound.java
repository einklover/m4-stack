package com.murphy.m4screenbridge.browser.stream;

/**
 * Non-blocking sink for already-encoded M4B3 packets.
 * Implementations must not perform socket I/O on the caller thread.
 */
public interface M4B3Outbound {
    void enqueue(byte[] packet);
}
