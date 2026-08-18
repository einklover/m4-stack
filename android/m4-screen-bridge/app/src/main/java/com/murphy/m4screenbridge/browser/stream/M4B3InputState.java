package com.murphy.m4screenbridge.browser.stream;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Host-testable single-pointer state for M4B3 TOUCH. Produces MotionEvent
 * action/order/x/y/downTime/eventTime without importing android.view.
 */
public final class M4B3InputState {
    public static final int ACTION_DOWN = 0;
    public static final int ACTION_UP = 1;
    public static final int ACTION_MOVE = 2;
    public static final int ACTION_CANCEL = 3;

    public static final class Dispatch {
        public final int motionAction;
        public final int x;
        public final int y;
        public final long downTime;
        public final long eventTime;
        public final long inputSeq;
        public final long session;
        public final boolean synthesized;

        Dispatch(int motionAction, int x, int y, long downTime, long eventTime,
                long inputSeq, long session, boolean synthesized) {
            this.motionAction = motionAction;
            this.x = x;
            this.y = y;
            this.downTime = downTime;
            this.eventTime = eventTime;
            this.inputSeq = inputSeq;
            this.session = session;
            this.synthesized = synthesized;
        }
    }

    private boolean pointerDown;
    private int lastX;
    private int lastY;
    private long downUptime;
    private long downDeviceMs;
    private long session = -1;
    private boolean sessionSeen;
    private long lastInputSeq = -1;
    private int lastMotion = -1;

    private long downCount;
    private long moveCount;
    private long upCount;
    private long cancelCount;
    private long rejected;
    private long coalesced;
    private long sessionResets;
    private long outOfOrder;
    private long synthesizedCancel;
    private final List<String> log = new ArrayList<String>();

    public List<Dispatch> apply(M4B3Message.Touch touch, long uptimeNow) {
        List<Dispatch> out = new ArrayList<Dispatch>(2);
        if (touch == null) {
            rejected++;
            return out;
        }
        if (!M4B3.validTouchAction(touch.action)
                || touch.x < 0 || touch.x >= M4B3.WIDTH
                || touch.y < 0 || touch.y >= M4B3.HEIGHT) {
            rejected++;
            return out;
        }
        if (sessionSeen && touch.session != session) {
            Dispatch cancel = synthesizeCancel(uptimeNow, true);
            if (cancel != null) out.add(cancel);
            session = touch.session;
            sessionSeen = true;
            sessionResets++;
            lastInputSeq = -1;
        } else if (!sessionSeen) {
            session = touch.session;
            sessionSeen = true;
        }
        if (touch.action == M4B3.TOUCH_DOWN && pointerDown) {
            Dispatch cancel = synthesizeCancel(uptimeNow, true);
            if (cancel != null) out.add(cancel);
        }
        Dispatch next = applySameSession(touch, uptimeNow);
        if (next != null) out.add(next);
        return out;
    }

    public Dispatch onTransportLost(long uptimeNow) {
        Dispatch cancel = synthesizeCancel(uptimeNow, true);
        sessionSeen = false;
        session = -1;
        lastInputSeq = -1;
        sessionResets++;
        return cancel;
    }

    public void reset() {
        pointerDown = false;
        sessionSeen = false;
        session = -1;
        lastInputSeq = -1;
        lastMotion = -1;
        downUptime = 0;
        downDeviceMs = 0;
    }

    public boolean pointerDown() { return pointerDown; }
    public int lastX() { return lastX; }
    public int lastY() { return lastY; }
    public long downCount() { return downCount; }
    public long moveCount() { return moveCount; }
    public long upCount() { return upCount; }
    public long cancelCount() { return cancelCount; }
    public long rejected() { return rejected; }
    public long coalesced() { return coalesced; }
    public long sessionResets() { return sessionResets; }
    public long outOfOrder() { return outOfOrder; }
    public long synthesizedCancel() { return synthesizedCancel; }
    public long session() { return session; }

    public String snapshot() {
        return String.format(Locale.ROOT,
                "in down=%d move=%d up=%d cancel=%d rej=%d coal=%d sessReset=%d ooo=%d synCancel=%d "
                        + "ptr=%d last=%d,%d sess=%d seq=%d",
                downCount, moveCount, upCount, cancelCount, rejected, coalesced, sessionResets,
                outOfOrder, synthesizedCancel, pointerDown ? 1 : 0, lastX, lastY, session,
                lastInputSeq);
    }

    public List<String> copyLog() {
        return new ArrayList<String>(log);
    }

    private Dispatch applySameSession(M4B3Message.Touch touch, long uptimeNow) {
        if (lastInputSeq >= 0 && touch.inputSeq < lastInputSeq) {
            outOfOrder++;
        }
        lastInputSeq = touch.inputSeq;
        switch (touch.action) {
            case M4B3.TOUCH_DOWN:
                return onDown(touch, uptimeNow);
            case M4B3.TOUCH_MOVE:
                return onMove(touch, uptimeNow);
            case M4B3.TOUCH_UP:
                return onTerminal(touch, uptimeNow, ACTION_UP, false);
            case M4B3.TOUCH_CANCEL:
                return onTerminal(touch, uptimeNow, ACTION_CANCEL, false);
            default:
                rejected++;
                return null;
        }
    }

    private Dispatch onDown(M4B3Message.Touch touch, long uptimeNow) {
        pointerDown = true;
        lastX = touch.x;
        lastY = touch.y;
        downUptime = uptimeNow;
        downDeviceMs = touch.tMs;
        lastMotion = ACTION_DOWN;
        downCount++;
        Dispatch d = new Dispatch(ACTION_DOWN, touch.x, touch.y, downUptime, downUptime,
                touch.inputSeq, touch.session, false);
        note(d);
        return d;
    }

    private Dispatch onMove(M4B3Message.Touch touch, long uptimeNow) {
        if (!pointerDown) {
            rejected++;
            return null;
        }
        if (lastMotion == ACTION_MOVE) coalesced++;
        lastX = touch.x;
        lastY = touch.y;
        lastMotion = ACTION_MOVE;
        moveCount++;
        Dispatch d = new Dispatch(ACTION_MOVE, touch.x, touch.y, downUptime,
                eventTime(touch.tMs, uptimeNow), touch.inputSeq, touch.session, false);
        note(d);
        return d;
    }

    private Dispatch onTerminal(M4B3Message.Touch touch, long uptimeNow, int motion, boolean synth) {
        if (!pointerDown) {
            rejected++;
            return null;
        }
        lastX = touch.x;
        lastY = touch.y;
        pointerDown = false;
        lastMotion = motion;
        if (motion == ACTION_UP) upCount++;
        else cancelCount++;
        Dispatch d = new Dispatch(motion, touch.x, touch.y, downUptime,
                eventTime(touch.tMs, uptimeNow), touch.inputSeq, touch.session, synth);
        note(d);
        return d;
    }

    private Dispatch synthesizeCancel(long uptimeNow, boolean countReset) {
        if (!pointerDown) return null;
        pointerDown = false;
        lastMotion = ACTION_CANCEL;
        cancelCount++;
        synthesizedCancel++;
        Dispatch d = new Dispatch(ACTION_CANCEL, lastX, lastY, downUptime, uptimeNow,
                lastInputSeq < 0 ? 0 : lastInputSeq, session, true);
        note(d);
        return d;
    }

    private long eventTime(long deviceMs, long uptimeNow) {
        long delta = deviceMs - downDeviceMs;
        if (delta < 0) delta = 0;
        long t = downUptime + delta;
        if (t < downUptime) return downUptime;
        if (t > uptimeNow + 60_000L) return uptimeNow;
        return t;
    }

    private void note(Dispatch d) {
        String name = d.motionAction == ACTION_DOWN ? "DOWN"
                : d.motionAction == ACTION_MOVE ? "MOVE"
                : d.motionAction == ACTION_UP ? "UP" : "CANCEL";
        String line = name + " " + d.x + "," + d.y + " t=" + d.eventTime;
        log.add(line);
        if (log.size() > 24) log.remove(0);
    }
}
