package com.murphy.m4screenbridge.browser.stream;

import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchRect;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

/** Strict little-endian M4B3 codec. Rejects overflow/truncation before allocation. */
public final class M4B3Codec {
    private M4B3Codec() {}

    public static byte[] encodeHello(M4B3Message.Hello hello, long seq) {
        if (hello == null) throw M4B3Exception.invalid("hello is null");
        ByteBuffer header = le(M4B3.HELLO_HEADER_SIZE);
        header.put((byte) hello.version);
        header.putShort((short) hello.width);
        header.putShort((short) hello.height);
        header.put((byte) hello.pixelFormat);
        header.putShort((short) hello.stride);
        header.putInt(hello.capabilities);
        header.putInt(hello.maxPayload);
        header.put((byte) hello.status);
        return wrap(M4B3.TYPE_HELLO, 0, header.array(), new byte[0], seq);
    }

    public static byte[] encodeV1Hello(long seq, int status) {
        return encodeHello(new M4B3Message.Hello(
                M4B3.VERSION, M4B3.WIDTH, M4B3.HEIGHT, M4B3.PIXEL_MONO1, M4B3.STRIDE,
                M4B3.V1_CAPABILITIES, M4B3.MAX_PAYLOAD_LEN, status), seq);
    }

    public static byte[] encodeKeyframe(long frameId, byte[] framebuffer, long seq) {
        LogicalMonoFrame.validate(framebuffer);
        int crc = M4B3.framebufferCrc32(framebuffer);
        ByteBuffer header = le(M4B3.KEY_HEADER_SIZE);
        putU32(header, frameId);
        header.putShort((short) M4B3.WIDTH);
        header.putShort((short) M4B3.HEIGHT);
        header.put((byte) M4B3.PIXEL_MONO1);
        header.putShort((short) M4B3.STRIDE);
        header.putInt(M4B3.KEYFRAME_SIZE);
        header.putInt(crc);
        return wrap(M4B3.TYPE_FRAME_KEY, 0, header.array(), framebuffer, seq);
    }

    public static byte[] encodePatch(FramePatch patch, byte[] finalFramebuffer, long seq) {
        if (patch == null) throw M4B3Exception.invalid("patch is null");
        if (patch.keyframe) {
            return encodeKeyframe(patch.frameId, finalFramebuffer, seq);
        }
        LogicalMonoFrame.validate(finalFramebuffer);
        if (patch.rects.size() > M4B3.MAX_RECT_COUNT) {
            throw M4B3Exception.oversized("rect_count " + patch.rects.size());
        }
        long payloadBytes = 0L;
        for (PatchRect rect : patch.rects) {
            payloadBytes += (long) M4B3.RECT_META_SIZE + rect.payloadBytes();
            if (payloadBytes > M4B3.MAX_PAYLOAD_LEN) {
                throw M4B3Exception.oversized("patch payload " + payloadBytes);
            }
        }
        byte[] payload = new byte[(int) payloadBytes];
        ByteBuffer pb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        for (PatchRect rect : patch.rects) {
            pb.putShort((short) rect.x);
            pb.putShort((short) rect.y);
            pb.putShort((short) rect.width);
            pb.putShort((short) rect.height);
            pb.putInt(rect.payloadBytes());
            pb.put(rect.data);
        }
        int crc = M4B3.framebufferCrc32(finalFramebuffer);
        ByteBuffer header = le(M4B3.PATCH_HEADER_SIZE);
        putU32(header, patch.frameId);
        putU32(header, patch.baseFrameId);
        header.putShort((short) patch.rects.size());
        header.putInt(crc);
        return wrap(M4B3.TYPE_FRAME_PATCH, 0, header.array(), payload, seq);
    }

    public static byte[] encodeAck(long frameId, int result, long acceptedFrameId, long seq) {
        ByteBuffer header = le(M4B3.ACK_HEADER_SIZE);
        putU32(header, frameId);
        header.put((byte) result);
        header.putInt((int) acceptedFrameId);
        return wrap(M4B3.TYPE_FRAME_ACK, 0, header.array(), new byte[0], seq);
    }

    public static byte[] encodePing(long nonce, long seq) {
        return encodePingPong(M4B3.TYPE_PING, nonce, seq);
    }

    public static byte[] encodePong(long nonce, long seq) {
        return encodePingPong(M4B3.TYPE_PONG, nonce, seq);
    }

    public static byte[] encodeInputKey(M4B3Message.InputKey key, long seq) {
        if (key == null) throw M4B3Exception.invalid("input key is null");
        if (!M4B3.validInputKeyAction(key.action)) {
            throw M4B3Exception.invalid("input key action " + key.action);
        }
        ByteBuffer header = le(M4B3.INPUT_KEY_HEADER_SIZE);
        header.put((byte) key.action);
        header.put((byte) key.flags);
        header.putShort((short) 0);
        putU32(header, key.tMs);
        putU32(header, key.inputSeq);
        putU32(header, key.session);
        return wrap(M4B3.TYPE_INPUT_KEY, 0, header.array(), new byte[0], seq);
    }

    public static byte[] encodeTouch(M4B3Message.Touch touch, long seq) {
        if (touch == null) throw M4B3Exception.invalid("touch is null");
        if (!M4B3.validTouchAction(touch.action)) {
            throw M4B3Exception.invalid("touch action " + touch.action);
        }
        if (touch.x < 0 || touch.x >= M4B3.WIDTH || touch.y < 0 || touch.y >= M4B3.HEIGHT) {
            throw M4B3Exception.invalid("touch xy " + touch.x + "," + touch.y);
        }
        ByteBuffer header = le(M4B3.TOUCH_HEADER_SIZE);
        header.put((byte) touch.action);
        header.put((byte) touch.flags);
        header.putShort((short) 0);
        header.putShort((short) touch.x);
        header.putShort((short) touch.y);
        putU32(header, touch.tMs);
        putU32(header, touch.inputSeq);
        putU32(header, touch.session);
        return wrap(M4B3.TYPE_TOUCH, 0, header.array(), new byte[0], seq);
    }

    /**
     * Builds a raw envelope. Length fields are taken from the supplied arrays unless
     * the optional overrides are non-null (used by tests to craft lying lengths).
     */
    public static byte[] wrap(int type, int flags, byte[] header, byte[] payload, long seq) {
        return wrap(type, flags, header, payload, seq, null, null);
    }

    public static byte[] wrap(int type, int flags, byte[] header, byte[] payload, long seq,
            Integer headerLenOverride, Long payloadLenOverride) {
        if (header == null) header = new byte[0];
        if (payload == null) payload = new byte[0];
        int headerLen = headerLenOverride != null ? headerLenOverride : header.length;
        long payloadLen = payloadLenOverride != null ? payloadLenOverride : payload.length;
        if (headerLen < 0 || payloadLen < 0) {
            throw M4B3Exception.invalid("negative length");
        }
        long total = (long) M4B3.ENVELOPE_SIZE + header.length + payload.length;
        if (total > Integer.MAX_VALUE) throw M4B3Exception.overflow("encoded message too large");
        ByteBuffer out = le((int) total);
        out.put(M4B3.MAGIC);
        out.put((byte) type);
        out.put((byte) flags);
        out.putShort((short) headerLen);
        out.putInt((int) payloadLen);
        putU32(out, seq);
        out.put(header);
        out.put(payload);
        return out.array();
    }

    public static M4B3Message parse(byte[] data) {
        if (data == null) throw M4B3Exception.truncated("message is null");
        return parse(data, 0, data.length);
    }

    public static M4B3Message parse(byte[] data, int offset, int length) {
        if (data == null) throw M4B3Exception.truncated("message is null");
        if (offset < 0 || length < 0 || offset > data.length || length > data.length - offset) {
            throw M4B3Exception.truncated("slice out of bounds");
        }
        if (length < M4B3.ENVELOPE_SIZE) {
            throw M4B3Exception.truncated("envelope shorter than " + M4B3.ENVELOPE_SIZE);
        }
        ByteBuffer bb = ByteBuffer.wrap(data, offset, length).order(ByteOrder.LITTLE_ENDIAN);
        byte m0 = bb.get();
        byte m1 = bb.get();
        byte m2 = bb.get();
        byte m3 = bb.get();
        if (m0 != M4B3.MAGIC[0] || m1 != M4B3.MAGIC[1]
                || m2 != M4B3.MAGIC[2] || m3 != M4B3.MAGIC[3]) {
            throw M4B3Exception.invalid("bad magic");
        }
        int type = bb.get() & 0xFF;
        int flags = bb.get() & 0xFF;
        int headerLen = bb.getShort() & 0xFFFF;
        long payloadLen = bb.getInt() & 0xFFFFFFFFL;
        long seq = bb.getInt() & 0xFFFFFFFFL;

        if (headerLen > M4B3.MAX_HEADER_LEN) {
            throw M4B3Exception.oversized("header_len=" + headerLen);
        }
        if (payloadLen > M4B3.MAX_PAYLOAD_LEN) {
            throw M4B3Exception.oversized("payload_len=" + payloadLen);
        }
        long body = (long) headerLen + payloadLen;
        if (body > (long) M4B3.MAX_HEADER_LEN + M4B3.MAX_PAYLOAD_LEN) {
            throw M4B3Exception.overflow("header+payload " + body);
        }
        long need = (long) M4B3.ENVELOPE_SIZE + body;
        if (need > length) {
            throw M4B3Exception.truncated("need " + need + " bytes, have " + length);
        }
        if (need != length) {
            throw M4B3Exception.invalid("trailing bytes after message: " + (length - need));
        }
        if (!M4B3.isKnownType(type)) {
            throw M4B3Exception.invalid("unknown type " + type);
        }

        byte[] header = new byte[headerLen];
        bb.get(header);
        int payloadOff = offset + M4B3.ENVELOPE_SIZE + headerLen;
        return parseBody(type, flags, seq, header, data, payloadOff, (int) payloadLen);
    }

    private static M4B3Message parseBody(int type, int flags, long seq, byte[] header,
            byte[] data, int payloadOff, int payloadLen) {
        switch (type) {
            case M4B3.TYPE_HELLO:
                return M4B3Message.hello(flags, seq, parseHello(header, payloadLen));
            case M4B3.TYPE_FRAME_KEY:
                return M4B3Message.keyframe(flags, seq,
                        parseKeyframe(header, data, payloadOff, payloadLen));
            case M4B3.TYPE_FRAME_PATCH:
                return M4B3Message.patch(flags, seq,
                        parsePatch(header, data, payloadOff, payloadLen));
            case M4B3.TYPE_FRAME_ACK:
                return M4B3Message.ack(flags, seq, parseAck(header, payloadLen));
            case M4B3.TYPE_PING:
            case M4B3.TYPE_PONG:
                return parsePingPong(type, flags, seq, header, payloadLen);
            case M4B3.TYPE_TOUCH:
                return M4B3Message.touch(flags, seq, parseTouch(header, payloadLen));
            case M4B3.TYPE_INPUT_KEY:
                return M4B3Message.inputKey(flags, seq, parseInputKey(header, payloadLen));
            default:
                throw M4B3Exception.invalid("unknown type " + type);
        }
    }

    private static M4B3Message.Hello parseHello(byte[] header, int payloadLen) {
        if (payloadLen != 0) throw M4B3Exception.invalid("HELLO payload must be empty");
        if (header.length != M4B3.HELLO_HEADER_SIZE) {
            throw M4B3Exception.invalid("HELLO header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int version = h.get() & 0xFF;
        int width = h.getShort() & 0xFFFF;
        int height = h.getShort() & 0xFFFF;
        int pixelFormat = h.get() & 0xFF;
        int stride = h.getShort() & 0xFFFF;
        int capabilities = h.getInt();
        int maxPayload = h.getInt();
        int status = h.get() & 0xFF;
        if (maxPayload < 0) throw M4B3Exception.overflow("HELLO max_payload");
        return new M4B3Message.Hello(version, width, height, pixelFormat, stride,
                capabilities, maxPayload, status);
    }

    private static M4B3Message.Keyframe parseKeyframe(byte[] header, byte[] data,
            int payloadOff, int payloadLen) {
        if (header.length != M4B3.KEY_HEADER_SIZE) {
            throw M4B3Exception.invalid("FRAME_KEY header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long frameId = h.getInt() & 0xFFFFFFFFL;
        int width = h.getShort() & 0xFFFF;
        int height = h.getShort() & 0xFFFF;
        int pixelFormat = h.get() & 0xFF;
        int stride = h.getShort() & 0xFFFF;
        long claimedPayload = h.getInt() & 0xFFFFFFFFL;
        int crc32 = h.getInt();
        if (claimedPayload != (payloadLen & 0xFFFFFFFFL)) {
            throw M4B3Exception.invalid("FRAME_KEY payload_len mismatch");
        }
        if (!M4B3.sameLogicalFormat(width, height, pixelFormat, stride)) {
            throw M4B3Exception.invalid("FRAME_KEY format " + width + "x" + height
                    + " fmt=" + pixelFormat + " stride=" + stride);
        }
        if (claimedPayload != M4B3.KEYFRAME_SIZE) {
            throw M4B3Exception.invalid("FRAME_KEY payload must be " + M4B3.KEYFRAME_SIZE);
        }
        byte[] payload = new byte[M4B3.KEYFRAME_SIZE];
        System.arraycopy(data, payloadOff, payload, 0, M4B3.KEYFRAME_SIZE);
        return new M4B3Message.Keyframe(frameId, width, height, pixelFormat, stride, payload, crc32);
    }

    private static M4B3Message.Patch parsePatch(byte[] header, byte[] data,
            int payloadOff, int payloadLen) {
        if (header.length != M4B3.PATCH_HEADER_SIZE) {
            throw M4B3Exception.invalid("FRAME_PATCH header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long frameId = h.getInt() & 0xFFFFFFFFL;
        long baseFrameId = h.getInt() & 0xFFFFFFFFL;
        int rectCount = h.getShort() & 0xFFFF;
        int crc32 = h.getInt();
        if (rectCount > M4B3.MAX_RECT_COUNT) {
            throw M4B3Exception.oversized("rect_count=" + rectCount);
        }
        long minNeed = (long) rectCount * M4B3.RECT_META_SIZE;
        if (minNeed > payloadLen) {
            throw M4B3Exception.truncated("rect metadata needs " + minNeed
                    + " bytes, payload_len=" + payloadLen);
        }

        List<PatchRect> rects = new ArrayList<>(rectCount);
        int cursor = payloadOff;
        int remaining = payloadLen;
        for (int i = 0; i < rectCount; i++) {
            if (remaining < M4B3.RECT_META_SIZE) {
                throw M4B3Exception.truncated("rect " + i + " metadata truncated");
            }
            ByteBuffer rb = ByteBuffer.wrap(data, cursor, M4B3.RECT_META_SIZE)
                    .order(ByteOrder.LITTLE_ENDIAN);
            int x = rb.getShort() & 0xFFFF;
            int y = rb.getShort() & 0xFFFF;
            int width = rb.getShort() & 0xFFFF;
            int height = rb.getShort() & 0xFFFF;
            long rectPayload = rb.getInt() & 0xFFFFFFFFL;
            cursor += M4B3.RECT_META_SIZE;
            remaining -= M4B3.RECT_META_SIZE;

            if ((x & 7) != 0 || (width & 7) != 0) {
                throw M4B3Exception.invalid("rect " + i + " not byte-aligned");
            }
            if (width == 0 || height == 0) {
                throw M4B3Exception.invalid("rect " + i + " has empty geometry");
            }
            long expected = ((long) width >>> 3) * (long) height;
            if (expected > Integer.MAX_VALUE || expected > M4B3.MAX_PAYLOAD_LEN) {
                throw M4B3Exception.overflow("rect " + i + " payload " + expected);
            }
            long right = (long) x + width;
            long bottom = (long) y + height;
            if (right > M4B3.WIDTH || bottom > M4B3.HEIGHT) {
                throw M4B3Exception.invalid("rect " + i + " outside 480x800");
            }
            if (rectPayload != expected) {
                throw M4B3Exception.invalid("rect " + i + " payload_len=" + rectPayload
                        + " expected=" + expected);
            }
            if (rectPayload > remaining) {
                throw M4B3Exception.truncated("rect " + i + " payload truncated");
            }
            byte[] rectData = new byte[(int) expected];
            System.arraycopy(data, cursor, rectData, 0, (int) expected);
            try {
                rects.add(new PatchRect(x, y, width, height, rectData));
            } catch (IllegalArgumentException e) {
                throw M4B3Exception.invalid("rect " + i + ": " + e.getMessage());
            }
            cursor += (int) expected;
            remaining -= (int) expected;
        }
        if (remaining != 0) {
            throw M4B3Exception.invalid("FRAME_PATCH trailing payload " + remaining);
        }
        return new M4B3Message.Patch(frameId, baseFrameId, crc32, rects);
    }

    private static M4B3Message.Ack parseAck(byte[] header, int payloadLen) {
        if (payloadLen != 0) throw M4B3Exception.invalid("FRAME_ACK payload must be empty");
        if (header.length != M4B3.ACK_HEADER_SIZE) {
            throw M4B3Exception.invalid("FRAME_ACK header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long frameId = h.getInt() & 0xFFFFFFFFL;
        int result = h.get() & 0xFF;
        int accepted = h.getInt();
        return new M4B3Message.Ack(frameId, result, accepted);
    }

    private static M4B3Message parsePingPong(int type, int flags, long seq,
            byte[] header, int payloadLen) {
        if (payloadLen != 0) throw M4B3Exception.invalid("PING/PONG payload must be empty");
        if (header.length != M4B3.PING_HEADER_SIZE) {
            throw M4B3Exception.invalid("PING/PONG header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long nonce = h.getInt() & 0xFFFFFFFFL;
        return type == M4B3.TYPE_PING
                ? M4B3Message.ping(flags, seq, nonce)
                : M4B3Message.pong(flags, seq, nonce);
    }

    private static M4B3Message.InputKey parseInputKey(byte[] header, int payloadLen) {
        if (payloadLen != 0) throw M4B3Exception.invalid("INPUT_KEY payload must be empty");
        if (header.length != M4B3.INPUT_KEY_HEADER_SIZE) {
            throw M4B3Exception.invalid("INPUT_KEY header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int action = h.get() & 0xFF;
        int flags = h.get() & 0xFF;
        h.getShort();
        long tMs = h.getInt() & 0xFFFFFFFFL;
        long inputSeq = h.getInt() & 0xFFFFFFFFL;
        long session = h.getInt() & 0xFFFFFFFFL;
        if (!M4B3.validInputKeyAction(action)) {
            throw M4B3Exception.invalid("INPUT_KEY action " + action);
        }
        return new M4B3Message.InputKey(action, flags, tMs, inputSeq, session);
    }

    private static M4B3Message.Touch parseTouch(byte[] header, int payloadLen) {
        if (payloadLen != 0) throw M4B3Exception.invalid("TOUCH payload must be empty");
        if (header.length != M4B3.TOUCH_HEADER_SIZE) {
            throw M4B3Exception.invalid("TOUCH header_len=" + header.length);
        }
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int action = h.get() & 0xFF;
        int flags = h.get() & 0xFF;
        h.getShort();
        int x = h.getShort() & 0xFFFF;
        int y = h.getShort() & 0xFFFF;
        long tMs = h.getInt() & 0xFFFFFFFFL;
        long inputSeq = h.getInt() & 0xFFFFFFFFL;
        long session = h.getInt() & 0xFFFFFFFFL;
        if (!M4B3.validTouchAction(action)) {
            throw M4B3Exception.invalid("TOUCH action " + action);
        }
        if (x >= M4B3.WIDTH || y >= M4B3.HEIGHT) {
            throw M4B3Exception.invalid("TOUCH xy " + x + "," + y);
        }
        return new M4B3Message.Touch(action, flags, x, y, tMs, inputSeq, session);
    }

    private static byte[] encodePingPong(int type, long nonce, long seq) {
        ByteBuffer header = le(M4B3.PING_HEADER_SIZE);
        putU32(header, nonce);
        return wrap(type, 0, header.array(), new byte[0], seq);
    }

    private static ByteBuffer le(int size) {
        return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static void putU32(ByteBuffer bb, long value) {
        if (value < 0 || value > 0xFFFFFFFFL) {
            throw M4B3Exception.overflow("u32 out of range: " + value);
        }
        bb.putInt((int) value);
    }
}
