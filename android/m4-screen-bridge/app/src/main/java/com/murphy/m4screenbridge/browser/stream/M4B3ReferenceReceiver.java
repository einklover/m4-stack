package com.murphy.m4screenbridge.browser.stream;

import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchApplier;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Host/reference M4B3 receiver. Applies keyframes/patches onto a working copy,
 * verifies the claimed final CRC, and only then commits. Failures never mutate
 * the accepted framebuffer.
 */
public final class M4B3ReferenceReceiver {
    private boolean helloAccepted;
    private long acceptedFrameId = -1;
    private int acceptedCrc;
    private byte[] framebuffer;
    private long nextSeq;
    private long keysApplied;
    private long patchesApplied;
    private long nacksSent;
    private int lastNackResult = -1;

    public List<byte[]> handle(byte[] packet) {
        final M4B3Message msg;
        try {
            msg = M4B3Codec.parse(packet);
        } catch (M4B3Exception e) {
            return Collections.emptyList();
        }
        switch (msg.type) {
            case M4B3.TYPE_HELLO:
                return singleton(handleHello(msg.hello));
            case M4B3.TYPE_FRAME_KEY:
                return singleton(handleKey(msg.keyframe));
            case M4B3.TYPE_FRAME_PATCH:
                return singleton(handlePatch(msg.patch));
            case M4B3.TYPE_PING:
                return singleton(M4B3Codec.encodePong(msg.nonce, nextSeq++));
            case M4B3.TYPE_PONG:
            case M4B3.TYPE_FRAME_ACK:
                return Collections.emptyList();
            default:
                return Collections.emptyList();
        }
    }

    public boolean isHelloAccepted() {
        return helloAccepted;
    }

    public long acceptedFrameId() {
        return acceptedFrameId;
    }

    public int acceptedCrc() {
        return acceptedCrc;
    }

    public byte[] copyFramebuffer() {
        return framebuffer == null ? null : Arrays.copyOf(framebuffer, framebuffer.length);
    }

    public long keysApplied() {
        return keysApplied;
    }

    public long patchesApplied() {
        return patchesApplied;
    }

    public long nacksSent() {
        return nacksSent;
    }

    public int lastNackResult() {
        return lastNackResult;
    }

    public void reset() {
        helloAccepted = false;
        acceptedFrameId = -1;
        acceptedCrc = 0;
        framebuffer = null;
        nextSeq = 0;
        keysApplied = 0;
        patchesApplied = 0;
        nacksSent = 0;
        lastNackResult = -1;
    }

    private byte[] handleHello(M4B3Message.Hello hello) {
        int status = M4B3.HELLO_OK;
        if (hello.version != M4B3.VERSION) {
            status = M4B3.HELLO_UNSUPPORTED_VERSION;
            helloAccepted = false;
        } else if (!M4B3.sameLogicalFormat(hello.width, hello.height, hello.pixelFormat, hello.stride)) {
            status = M4B3.HELLO_FORMAT_MISMATCH;
            helloAccepted = false;
        } else {
            helloAccepted = true;
        }
        return M4B3Codec.encodeV1Hello(nextSeq++, status);
    }

    private byte[] handleKey(M4B3Message.Keyframe key) {
        byte[] working;
        try {
            working = PatchApplier.apply(null, -1, key.toFramePatch());
        } catch (RuntimeException e) {
            return nack(key.frameId, M4B3.NACK_MALFORMED);
        }
        int crc = M4B3.framebufferCrc32(working);
        if (crc != key.crc32) {
            return nack(key.frameId, M4B3.NACK_CRC);
        }
        commit(working, key.frameId, crc);
        keysApplied++;
        return ackOk(key.frameId);
    }

    private byte[] handlePatch(M4B3Message.Patch patch) {
        if (framebuffer == null || acceptedFrameId != patch.baseFrameId) {
            return nack(patch.frameId, M4B3.NACK_BASE);
        }
        byte[] working;
        try {
            working = PatchApplier.apply(framebuffer, acceptedFrameId, patch.toFramePatch());
        } catch (RuntimeException e) {
            return nack(patch.frameId, M4B3.NACK_MALFORMED);
        }
        int crc = M4B3.framebufferCrc32(working);
        if (crc != patch.crc32) {
            return nack(patch.frameId, M4B3.NACK_CRC);
        }
        commit(working, patch.frameId, crc);
        patchesApplied++;
        return ackOk(patch.frameId);
    }

    private void commit(byte[] working, long frameId, int crc) {
        framebuffer = working;
        acceptedFrameId = frameId;
        acceptedCrc = crc;
        helloAccepted = true;
    }

    private byte[] ackOk(long frameId) {
        return M4B3Codec.encodeAck(frameId, M4B3.ACK_OK, acceptedFrameId, nextSeq++);
    }

    private byte[] nack(long frameId, int result) {
        nacksSent++;
        lastNackResult = result;
        return M4B3Codec.encodeAck(frameId, result, acceptedFrameId, nextSeq++);
    }

    private static List<byte[]> singleton(byte[] packet) {
        List<byte[]> out = new ArrayList<>(1);
        out.add(packet);
        return out;
    }
}
