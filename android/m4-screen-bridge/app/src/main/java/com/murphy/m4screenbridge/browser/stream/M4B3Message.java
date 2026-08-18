package com.murphy.m4screenbridge.browser.stream;

import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.PatchRect;

import java.util.Collections;
import java.util.List;

/** Parsed M4B3 message. Exactly one of the typed payloads is populated. */
public final class M4B3Message {
    public final int type;
    public final int flags;
    public final long seq;
    public final Hello hello;
    public final Keyframe keyframe;
    public final Patch patch;
    public final Ack ack;
    public final Touch touch;
    public final long nonce;

    private M4B3Message(int type, int flags, long seq, Hello hello, Keyframe keyframe,
            Patch patch, Ack ack, Touch touch, long nonce) {
        this.type = type;
        this.flags = flags;
        this.seq = seq;
        this.hello = hello;
        this.keyframe = keyframe;
        this.patch = patch;
        this.ack = ack;
        this.touch = touch;
        this.nonce = nonce;
    }

    public static M4B3Message hello(int flags, long seq, Hello hello) {
        return new M4B3Message(M4B3.TYPE_HELLO, flags, seq, hello, null, null, null, null, 0);
    }

    public static M4B3Message keyframe(int flags, long seq, Keyframe keyframe) {
        return new M4B3Message(M4B3.TYPE_FRAME_KEY, flags, seq, null, keyframe, null, null, null, 0);
    }

    public static M4B3Message patch(int flags, long seq, Patch patch) {
        return new M4B3Message(M4B3.TYPE_FRAME_PATCH, flags, seq, null, null, patch, null, null, 0);
    }

    public static M4B3Message ack(int flags, long seq, Ack ack) {
        return new M4B3Message(M4B3.TYPE_FRAME_ACK, flags, seq, null, null, null, ack, null, 0);
    }

    public static M4B3Message ping(int flags, long seq, long nonce) {
        return new M4B3Message(M4B3.TYPE_PING, flags, seq, null, null, null, null, null, nonce);
    }

    public static M4B3Message pong(int flags, long seq, long nonce) {
        return new M4B3Message(M4B3.TYPE_PONG, flags, seq, null, null, null, null, null, nonce);
    }

    public static M4B3Message touch(int flags, long seq, Touch touch) {
        return new M4B3Message(M4B3.TYPE_TOUCH, flags, seq, null, null, null, null, touch, 0);
    }

    public boolean isFrame() {
        return type == M4B3.TYPE_FRAME_KEY || type == M4B3.TYPE_FRAME_PATCH;
    }

    public static final class Hello {
        public final int version;
        public final int width;
        public final int height;
        public final int pixelFormat;
        public final int stride;
        public final int capabilities;
        public final int maxPayload;
        public final int status;

        public Hello(int version, int width, int height, int pixelFormat, int stride,
                int capabilities, int maxPayload, int status) {
            this.version = version;
            this.width = width;
            this.height = height;
            this.pixelFormat = pixelFormat;
            this.stride = stride;
            this.capabilities = capabilities;
            this.maxPayload = maxPayload;
            this.status = status;
        }

        public boolean compatibleV1() {
            return status == M4B3.HELLO_OK
                    && version == M4B3.VERSION
                    && M4B3.sameLogicalFormat(width, height, pixelFormat, stride);
        }
    }

    public static final class Keyframe {
        public final long frameId;
        public final int width;
        public final int height;
        public final int pixelFormat;
        public final int stride;
        public final byte[] payload;
        public final int crc32;

        public Keyframe(long frameId, int width, int height, int pixelFormat, int stride,
                byte[] payload, int crc32) {
            this.frameId = frameId;
            this.width = width;
            this.height = height;
            this.pixelFormat = pixelFormat;
            this.stride = stride;
            this.payload = payload;
            this.crc32 = crc32;
        }

        public FramePatch toFramePatch() {
            PatchRect rect = new PatchRect(0, 0, M4B3.WIDTH, M4B3.HEIGHT, payload);
            return new FramePatch(frameId, -1, true, FrameDiffer.TOTAL_TILES,
                    FrameDiffer.TOTAL_TILES, Collections.singletonList(rect));
        }
    }

    public static final class Patch {
        public final long frameId;
        public final long baseFrameId;
        public final int crc32;
        public final List<PatchRect> rects;

        public Patch(long frameId, long baseFrameId, int crc32, List<PatchRect> rects) {
            this.frameId = frameId;
            this.baseFrameId = baseFrameId;
            this.crc32 = crc32;
            this.rects = Collections.unmodifiableList(rects);
        }

        public FramePatch toFramePatch() {
            return new FramePatch(frameId, baseFrameId, false, rects.size(),
                    Math.max(FrameDiffer.TOTAL_TILES, rects.size()), rects);
        }
    }

    public static final class Ack {
        public final long frameId;
        public final int result;
        public final long acceptedFrameId;

        public Ack(long frameId, int result, long acceptedFrameId) {
            this.frameId = frameId;
            this.result = result;
            this.acceptedFrameId = acceptedFrameId;
        }

        public boolean ok() {
            return result == M4B3.ACK_OK;
        }
    }

    public static final class Touch {
        public final int action;
        public final int flags;
        public final int x;
        public final int y;
        public final long tMs;
        public final long inputSeq;
        public final long session;

        public Touch(int action, int flags, int x, int y, long tMs, long inputSeq, long session) {
            this.action = action;
            this.flags = flags;
            this.x = x;
            this.y = y;
            this.tMs = tMs;
            this.inputSeq = inputSeq;
            this.session = session;
        }
    }
}
