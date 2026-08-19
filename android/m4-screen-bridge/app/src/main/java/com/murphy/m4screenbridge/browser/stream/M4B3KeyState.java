package com.murphy.m4screenbridge.browser.stream;

import java.util.Locale;

/**
 * Pure-Java acceptance filter for M4B3 Browser Bridge key-return events.
 * TCP preserves order, but the session/sequence checks keep reconnect semantics
 * explicit and prevent a stale queued key from affecting a newly owned WebView.
 */
public final class M4B3KeyState {
    private boolean sessionSeen;
    private long session = -1;
    private long lastInputSeq = -1;
    private long accepted;
    private long back;
    private long reload;
    private long rejected;
    private long duplicateOrOld;
    private long sessionResets;

    public boolean accept(M4B3Message.InputKey key) {
        if (key == null || !M4B3.validInputKeyAction(key.action)) {
            rejected++;
            return false;
        }
        if (!sessionSeen || key.session != session) {
            sessionSeen = true;
            session = key.session;
            lastInputSeq = -1;
            sessionResets++;
        }
        if (lastInputSeq >= 0 && key.inputSeq <= lastInputSeq) {
            duplicateOrOld++;
            return false;
        }
        lastInputSeq = key.inputSeq;
        accepted++;
        if (key.action == M4B3.INPUT_KEY_BACK) back++;
        else if (key.action == M4B3.INPUT_KEY_RELOAD) reload++;
        return true;
    }

    public void onTransportLost() {
        sessionSeen = false;
        session = -1;
        lastInputSeq = -1;
        sessionResets++;
    }

    public void reset() {
        sessionSeen = false;
        session = -1;
        lastInputSeq = -1;
    }

    public long accepted() { return accepted; }
    public long back() { return back; }
    public long reload() { return reload; }
    public long rejected() { return rejected; }
    public long duplicateOrOld() { return duplicateOrOld; }
    public long sessionResets() { return sessionResets; }
    public long session() { return session; }
    public long lastInputSeq() { return lastInputSeq; }

    public String snapshot() {
        return String.format(Locale.ROOT,
                "key accepted=%d back=%d reload=%d rej=%d old=%d sessReset=%d sess=%d seq=%d",
                accepted, back, reload, rejected, duplicateOrOld, sessionResets, session, lastInputSeq);
    }
}
