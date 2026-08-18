package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.stream.M4B3;
import com.murphy.m4screenbridge.browser.stream.M4B3Codec;
import com.murphy.m4screenbridge.browser.stream.M4B3Exception;
import com.murphy.m4screenbridge.browser.stream.M4B3InputState;
import com.murphy.m4screenbridge.browser.stream.M4B3Message;
import com.murphy.m4screenbridge.browser.stream.M4B3Sender;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Pure-Java M4B3 TOUCH codec + single-pointer dispatch state. */
public final class M4B3InputTest {
    public static void main(String[] args) {
        codecRoundtrip();
        malformedRejected();
        dispatchOrderAndTiming();
        moveCoalesceAndDisconnectCancel();
        sessionResetAndInvalidReject();
        senderIgnoresTouch();
        System.out.println("OK: M4B3 input self-checks passed");
    }

    private static void codecRoundtrip() {
        M4B3Message.Touch t = new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 0, 0, 10, 1, 2);
        byte[] wire = M4B3Codec.encodeTouch(t, 9);
        assertEquals('M', wire[0] & 0xFF, "magic0");
        assertEquals(M4B3.TYPE_TOUCH, wire[4] & 0xFF, "TOUCH type");
        assertEquals(M4B3.TOUCH_HEADER_SIZE, (wire[6] & 0xFF) | ((wire[7] & 0xFF) << 8),
                "header_len LE");
        M4B3Message parsed = M4B3Codec.parse(wire);
        assertEquals(M4B3.TYPE_TOUCH, parsed.type, "parsed type");
        assertEquals(M4B3.TOUCH_DOWN, parsed.touch.action, "action");
        assertEquals(0, parsed.touch.x, "x");
        assertEquals(0, parsed.touch.y, "y");
        assertEquals(10L, parsed.touch.tMs, "tMs");
        assertEquals(1L, parsed.touch.inputSeq, "inputSeq");
        assertEquals(2L, parsed.touch.session, "session");
        assertTrue(Arrays.equals(wire, M4B3Codec.encodeTouch(parsed.touch, parsed.seq)),
                "TOUCH encode/decode identity");

        int[][] pts = {
                {0, 0}, {479, 0}, {0, 799}, {479, 799}, {240, 400}, {80, 140}, {320, 456}
        };
        for (int[] p : pts) {
            M4B3Message.Touch pt = new M4B3Message.Touch(
                    M4B3.TOUCH_MOVE, 0, p[0], p[1], 1, 3, 1);
            M4B3Message again = M4B3Codec.parse(M4B3Codec.encodeTouch(pt, 1));
            assertEquals(p[0], again.touch.x, "fixture x " + p[0]);
            assertEquals(p[1], again.touch.y, "fixture y " + p[1]);
        }
    }

    private static void malformedRejected() {
        expectInvalid(new M4B3Message.Touch(0, 0, 10, 10, 0, 0, 1), "action 0");
        expectInvalid(new M4B3Message.Touch(5, 0, 10, 10, 0, 0, 1), "action 5");
        expectInvalid(new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 480, 0, 0, 0, 1), "x=480");
        expectInvalid(new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 0, 800, 0, 0, 1), "y=800");

        byte[] good = M4B3Codec.encodeTouch(
                new M4B3Message.Touch(M4B3.TOUCH_UP, 0, 1, 1, 0, 0, 1), 1);
        expectParseFail(Arrays.copyOf(good, 10), "truncated envelope");
        byte[] payload = M4B3Codec.wrap(M4B3.TYPE_TOUCH, 0,
                Arrays.copyOfRange(good, M4B3.ENVELOPE_SIZE, good.length), new byte[] {1}, 1);
        expectParseFail(payload, "non-empty payload");

        byte[] header = Arrays.copyOfRange(good, M4B3.ENVELOPE_SIZE, good.length);
        header[0] = 0;
        expectParseFail(M4B3Codec.wrap(M4B3.TYPE_TOUCH, 0, header, new byte[0], 1), "bad action");
        header[0] = M4B3.TOUCH_MOVE;
        header[4] = (byte) 0xE0;
        header[5] = 0x01; // x=480
        expectParseFail(M4B3Codec.wrap(M4B3.TYPE_TOUCH, 0, header, new byte[0], 1), "oor x");
    }

    private static void dispatchOrderAndTiming() {
        M4B3InputState st = new M4B3InputState();
        List<M4B3InputState.Dispatch> a = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 80, 140, 1000, 1, 1), 5000);
        assertEquals(1, a.size(), "down count");
        assertEquals(M4B3InputState.ACTION_DOWN, a.get(0).motionAction, "DOWN");
        assertEquals(80, a.get(0).x, "down x");
        assertEquals(140, a.get(0).y, "down y");
        assertEquals(5000L, a.get(0).downTime, "downTime");
        assertEquals(5000L, a.get(0).eventTime, "down eventTime");

        List<M4B3InputState.Dispatch> b = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_MOVE, 0, 90, 200, 1120, 2, 1), 5200);
        assertEquals(1, b.size(), "move count");
        assertEquals(M4B3InputState.ACTION_MOVE, b.get(0).motionAction, "MOVE");
        assertEquals(5000L, b.get(0).downTime, "move keeps downTime");
        assertEquals(5120L, b.get(0).eventTime, "eventTime = down + delta");

        List<M4B3InputState.Dispatch> c = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_UP, 0, 90, 200, 1300, 3, 1), 5400);
        assertEquals(1, c.size(), "up count");
        assertEquals(M4B3InputState.ACTION_UP, c.get(0).motionAction, "UP");
        assertEquals(5000L, c.get(0).downTime, "up keeps downTime");
        assertEquals(5300L, c.get(0).eventTime, "up eventTime");
        assertTrue(!st.pointerDown(), "pointer up");
        assertEquals(1L, st.downCount(), "one down");
        assertEquals(1L, st.upCount(), "one up");
    }

    private static void moveCoalesceAndDisconnectCancel() {
        M4B3InputState st = new M4B3InputState();
        st.apply(new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 10, 10, 0, 1, 1), 100);
        st.apply(new M4B3Message.Touch(M4B3.TOUCH_MOVE, 0, 11, 10, 10, 2, 1), 120);
        st.apply(new M4B3Message.Touch(M4B3.TOUCH_MOVE, 0, 12, 10, 20, 3, 1), 140);
        st.apply(new M4B3Message.Touch(M4B3.TOUCH_MOVE, 0, 13, 10, 30, 4, 1), 160);
        assertEquals(3L, st.moveCount(), "three moves applied");
        assertEquals(2L, st.coalesced(), "consecutive MOVE counted");

        M4B3InputState.Dispatch cancel = st.onTransportLost(200);
        assertTrue(cancel != null, "disconnect synthesizes CANCEL");
        assertEquals(M4B3InputState.ACTION_CANCEL, cancel.motionAction, "CANCEL");
        assertTrue(cancel.synthesized, "synthesized");
        assertTrue(!st.pointerDown(), "pointer cleared");
        assertEquals(1L, st.synthesizedCancel(), "one synth cancel");

        List<M4B3InputState.Dispatch> again = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 20, 20, 40, 1, 2), 300);
        assertEquals(1, again.size(), "next gesture after reset");
        assertEquals(M4B3InputState.ACTION_DOWN, again.get(0).motionAction, "fresh DOWN");
    }

    private static void sessionResetAndInvalidReject() {
        M4B3InputState st = new M4B3InputState();
        st.apply(new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 5, 5, 0, 1, 9), 100);
        List<M4B3InputState.Dispatch> next = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 40, 50, 10, 1, 10), 150);
        assertEquals(2, next.size(), "session change CANCEL then DOWN");
        assertEquals(M4B3InputState.ACTION_CANCEL, next.get(0).motionAction, "session CANCEL");
        assertEquals(M4B3InputState.ACTION_DOWN, next.get(1).motionAction, "new DOWN");
        assertEquals(1L, st.sessionResets(), "session reset counted");

        long rej = st.rejected();
        assertTrue(st.apply(new M4B3Message.Touch(0, 0, 1, 1, 0, 2, 10), 160).isEmpty(),
                "bad action rejected");
        assertTrue(st.apply(new M4B3Message.Touch(M4B3.TOUCH_MOVE, 0, 480, 0, 0, 3, 10), 170)
                .isEmpty(), "oor rejected");
        assertTrue(st.apply(null, 180).isEmpty(), "null rejected");
        assertTrue(st.rejected() >= rej + 3, "rejects increment");

        List<M4B3InputState.Dispatch> stacked = st.apply(
                new M4B3Message.Touch(M4B3.TOUCH_DOWN, 0, 8, 8, 20, 4, 10), 200);
        assertEquals(2, stacked.size(), "DOWN while active CANCEL+DOWN");
        assertEquals(M4B3InputState.ACTION_CANCEL, stacked.get(0).motionAction, "implicit CANCEL");
    }

    private static void senderIgnoresTouch() {
        List<byte[]> sent = new ArrayList<byte[]>();
        M4B3Sender sender = new M4B3Sender(sent::add);
        sender.connect();
        byte[] touch = M4B3Codec.encodeTouch(
                new M4B3Message.Touch(M4B3.TOUCH_UP, 0, 1, 1, 0, 0, 1), 3);
        sender.receive(touch);
        for (byte[] pkt : sent) {
            assertTrue(M4B3Codec.parse(pkt).type != M4B3.TYPE_TOUCH, "sender does not echo TOUCH");
        }
    }

    private static void expectInvalid(M4B3Message.Touch t, String msg) {
        try {
            M4B3Codec.encodeTouch(t, 1);
            throw new AssertionError("expected invalid: " + msg);
        } catch (M4B3Exception e) {
            assertEquals(M4B3Exception.Kind.INVALID, e.kind, msg + " kind");
        }
    }

    private static void expectParseFail(byte[] wire, String msg) {
        try {
            M4B3Codec.parse(wire);
            throw new AssertionError("expected parse fail: " + msg);
        } catch (M4B3Exception e) {
            assertTrue(e.kind == M4B3Exception.Kind.INVALID
                    || e.kind == M4B3Exception.Kind.TRUNCATED
                    || e.kind == M4B3Exception.Kind.OVERSIZED, msg + " kind=" + e.kind);
        }
    }

    private static void assertTrue(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    private static void assertEquals(int expected, int actual, String msg) {
        if (expected != actual) {
            throw new AssertionError(msg + ": expected " + expected + " got " + actual);
        }
    }

    private static void assertEquals(long expected, long actual, String msg) {
        if (expected != actual) {
            throw new AssertionError(msg + ": expected " + expected + " got " + actual);
        }
    }

    private static void assertEquals(Object expected, Object actual, String msg) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(msg + ": expected " + expected + " got " + actual);
        }
    }
}
