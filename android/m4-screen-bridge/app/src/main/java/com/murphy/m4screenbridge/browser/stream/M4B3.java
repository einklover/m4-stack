package com.murphy.m4screenbridge.browser.stream;

import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;

import java.util.zip.CRC32;

/**
 * Versioned M4B3 constants for the Browser Bridge duplex stream.
 *
 * Envelope is always little-endian:
 * magic[4]="M4B3" type u8 flags u8 header_len u16 payload_len u32 seq u32
 * followed by header[header_len] and payload[payload_len].
 */
public final class M4B3 {
    public static final byte[] MAGIC = new byte[] {'M', '4', 'B', '3'};
    public static final int VERSION = 1;
    public static final int ENVELOPE_SIZE = 16;

    public static final int TYPE_HELLO = 1;
    public static final int TYPE_FRAME_KEY = 2;
    public static final int TYPE_FRAME_PATCH = 3;
    public static final int TYPE_FRAME_ACK = 4;
    public static final int TYPE_PING = 5;
    public static final int TYPE_PONG = 6;
    public static final int TYPE_TOUCH = 7;

    public static final int PIXEL_MONO1 = 1;
    public static final int WIDTH = LogicalMonoFrame.WIDTH;
    public static final int HEIGHT = LogicalMonoFrame.HEIGHT;
    public static final int STRIDE = LogicalMonoFrame.STRIDE;
    public static final int KEYFRAME_SIZE = LogicalMonoFrame.SIZE;

    public static final int HELLO_HEADER_SIZE = 17;
    public static final int KEY_HEADER_SIZE = 19;
    public static final int PATCH_HEADER_SIZE = 14;
    public static final int ACK_HEADER_SIZE = 9;
    public static final int PING_HEADER_SIZE = 4;
    public static final int TOUCH_HEADER_SIZE = 20;
    public static final int RECT_META_SIZE = 12;

    public static final int TOUCH_DOWN = 1;
    public static final int TOUCH_MOVE = 2;
    public static final int TOUCH_UP = 3;
    public static final int TOUCH_CANCEL = 4;

    /** Tight bound: reserved space only, never a large allocation ceiling. */
    public static final int MAX_HEADER_LEN = 32;
    /**
     * Worst-case patch payload is every 16x16 tile as its own rectangle:
     * 1500 * (12 + 32) = 66_000. 96 KiB leaves a small guard without allowing
     * multi-megabyte allocations.
     */
    public static final int MAX_PAYLOAD_LEN = 96 * 1024;
    public static final int MAX_RECT_COUNT = 1500;
    public static final int MAX_MESSAGE_SIZE = ENVELOPE_SIZE + MAX_HEADER_LEN + MAX_PAYLOAD_LEN;

    public static final int CAP_MONO1 = 1;
    public static final int CAP_PATCH = 1 << 1;
    public static final int V1_CAPABILITIES = CAP_MONO1 | CAP_PATCH;

    public static final int HELLO_OK = 0;
    public static final int HELLO_UNSUPPORTED_VERSION = 1;
    public static final int HELLO_FORMAT_MISMATCH = 2;

    public static final int ACK_OK = 0;
    public static final int NACK_CRC = 1;
    public static final int NACK_BASE = 2;
    public static final int NACK_MALFORMED = 3;
    public static final int NACK_OVERFLOW = 4;
    public static final int NACK_VERSION = 5;

    /** IEEE CRC-32 check value of the ASCII string "123456789". */
    public static final int CRC32_CHECK = 0xCBF43926;

    private M4B3() {}

    public static boolean isKnownType(int type) {
        return type >= TYPE_HELLO && type <= TYPE_TOUCH;
    }

    public static boolean validTouchAction(int action) {
        return action >= TOUCH_DOWN && action <= TOUCH_CANCEL;
    }

    public static String typeName(int type) {
        switch (type) {
            case TYPE_HELLO: return "HELLO";
            case TYPE_FRAME_KEY: return "FRAME_KEY";
            case TYPE_FRAME_PATCH: return "FRAME_PATCH";
            case TYPE_FRAME_ACK: return "FRAME_ACK";
            case TYPE_PING: return "PING";
            case TYPE_PONG: return "PONG";
            case TYPE_TOUCH: return "TOUCH";
            default: return "UNKNOWN(" + type + ")";
        }
    }

    public static int crc32(byte[] data) {
        return crc32(data, 0, data == null ? 0 : data.length);
    }

    public static int crc32(byte[] data, int off, int len) {
        if (data == null) throw new IllegalArgumentException("crc32 data is null");
        CRC32 crc = new CRC32();
        crc.update(data, off, len);
        return (int) crc.getValue();
    }

    public static int framebufferCrc32(byte[] frame) {
        LogicalMonoFrame.validate(frame);
        return crc32(frame);
    }

    public static boolean sameLogicalFormat(int width, int height, int pixelFormat, int stride) {
        return width == WIDTH && height == HEIGHT
                && pixelFormat == PIXEL_MONO1 && stride == STRIDE;
    }

    public static String crcHex(int crc) {
        return String.format("0x%08X", crc);
    }
}
