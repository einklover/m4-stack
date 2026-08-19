package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.stream.M4B3;
import com.murphy.m4screenbridge.browser.stream.M4B3Codec;
import com.murphy.m4screenbridge.browser.stream.M4B3Exception;
import com.murphy.m4screenbridge.browser.stream.M4B3KeyState;
import com.murphy.m4screenbridge.browser.stream.M4B3Message;

import java.util.Arrays;

/** Pure-Java Browser Bridge INPUT_KEY codec/session checks. */
public final class M4B3KeyTest {
    public static void main(String[] args) {
        codecRoundtrip();
        malformedRejected();
        sessionAndSequenceFilter();
        System.out.println("OK: M4B3 key-return self-checks passed");
    }

    private static void codecRoundtrip() {
        M4B3Message.InputKey key = new M4B3Message.InputKey(
                M4B3.INPUT_KEY_BACK, 0, 1234, 7, 3);
        byte[] wire = M4B3Codec.encodeInputKey(key, 21);
        assertEquals(M4B3.ENVELOPE_SIZE + M4B3.INPUT_KEY_HEADER_SIZE, wire.length, "wire size");
        assertEquals(M4B3.TYPE_INPUT_KEY, wire[4] & 0xFF, "type");
        assertEquals(M4B3.INPUT_KEY_HEADER_SIZE,
                (wire[6] & 0xFF) | ((wire[7] & 0xFF) << 8), "header size");
        M4B3Message parsed = M4B3Codec.parse(wire);
        assertEquals(M4B3.TYPE_INPUT_KEY, parsed.type, "parsed type");
        assertEquals(M4B3.INPUT_KEY_BACK, parsed.inputKey.action, "action");
        assertEquals(1234, parsed.inputKey.tMs, "time");
        assertEquals(7, parsed.inputKey.inputSeq, "input seq");
        assertEquals(3, parsed.inputKey.session, "session");

        M4B3Message.InputKey reload = new M4B3Message.InputKey(
                M4B3.INPUT_KEY_RELOAD, 2, 99, 8, 3);
        M4B3Message reloadParsed = M4B3Codec.parse(M4B3Codec.encodeInputKey(reload, 22));
        assertEquals(M4B3.INPUT_KEY_RELOAD, reloadParsed.inputKey.action, "reload action");
        assertEquals(2, reloadParsed.inputKey.flags, "flags");
    }

    private static void malformedRejected() {
        byte[] good = M4B3Codec.encodeInputKey(new M4B3Message.InputKey(
                M4B3.INPUT_KEY_BACK, 0, 1, 0, 1), 1);
        expectInvalid(Arrays.copyOf(good, good.length - 1), "truncated key");

        byte[] badAction = Arrays.copyOf(good, good.length);
        badAction[M4B3.ENVELOPE_SIZE] = 0;
        expectInvalid(badAction, "zero action");
        badAction[M4B3.ENVELOPE_SIZE] = 3;
        expectInvalid(badAction, "unknown action");

        byte[] payload = M4B3Codec.wrap(M4B3.TYPE_INPUT_KEY, 0,
                Arrays.copyOfRange(good, M4B3.ENVELOPE_SIZE, good.length),
                new byte[] {1}, 1);
        expectInvalid(payload, "payload forbidden");
    }

    private static void sessionAndSequenceFilter() {
        M4B3KeyState state = new M4B3KeyState();
        assertTrue(state.accept(new M4B3Message.InputKey(M4B3.INPUT_KEY_BACK, 0, 10, 0, 1)),
                "first key accepted");
        assertTrue(!state.accept(new M4B3Message.InputKey(M4B3.INPUT_KEY_BACK, 0, 11, 0, 1)),
                "duplicate seq rejected");
        assertTrue(state.accept(new M4B3Message.InputKey(M4B3.INPUT_KEY_RELOAD, 0, 12, 1, 1)),
                "next seq accepted");
        assertEquals(2, state.accepted(), "accepted count");
        assertEquals(1, state.back(), "back count");
        assertEquals(1, state.reload(), "reload count");
        assertEquals(1, state.duplicateOrOld(), "old count");

        // A firmware input-session change makes sequence zero valid again.
        assertTrue(state.accept(new M4B3Message.InputKey(M4B3.INPUT_KEY_BACK, 0, 20, 0, 2)),
                "new session resets sequence");
        assertEquals(2, state.session(), "session changed");
        state.onTransportLost();
        assertTrue(state.accept(new M4B3Message.InputKey(M4B3.INPUT_KEY_RELOAD, 0, 30, 0, 2)),
                "transport loss clears stale seq guard");
        assertTrue(state.snapshot().contains("reload=2"), "snapshot count");
    }

    private static void expectInvalid(byte[] wire, String message) {
        try {
            M4B3Codec.parse(wire);
            throw new AssertionError(message + " did not fail");
        } catch (M4B3Exception expected) {
            // Any strict parse failure is sufficient for these malformed fixtures.
        }
    }

    private static void assertTrue(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void assertEquals(long expected, long actual, String message) {
        if (expected != actual) {
            throw new AssertionError(message + ": expected=" + expected + " actual=" + actual);
        }
    }
}
