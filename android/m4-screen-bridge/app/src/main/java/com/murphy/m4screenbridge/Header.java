package com.murphy.m4screenbridge;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/** 24-byte little-endian M4 page header. Pure Java.
 *
 * magic "M4R1" (4), version u8=1, codec u8, width u16, height u16, stride u16,
 * page i32, rawSize u32, crc32 u32 (of decoded physical framebuffer). */
public final class Header {
    public static final int SIZE = 24;
    public static final String MAGIC = "M4R1";
    public static final int VERSION = 1;

    private Header() {}

    public static byte[] build(int codec, int width, int height, int stride,
                               int page, int rawSize, long crc32) {
        ByteBuffer bb = ByteBuffer.allocate(SIZE).order(ByteOrder.LITTLE_ENDIAN);
        bb.put(MAGIC.getBytes(StandardCharsets.US_ASCII));
        bb.put((byte) VERSION);
        bb.put((byte) codec);
        bb.putShort((short) width);
        bb.putShort((short) height);
        bb.putShort((short) stride);
        bb.putInt(page);
        bb.putInt(rawSize);
        bb.putInt((int) crc32);
        return bb.array();
    }

    /** Parses back into {version, codec, width, height, stride, page, rawSize, crc32}. */
    public static int[] parse(byte[] h) {
        if (h.length < SIZE) return null;
        ByteBuffer bb = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic = new byte[4];
        bb.get(magic);
        if (!MAGIC.equals(new String(magic, StandardCharsets.US_ASCII))) return null;
        int version = bb.get() & 0xFF;
        int codec = bb.get() & 0xFF;
        int width = bb.getShort() & 0xFFFF;
        int height = bb.getShort() & 0xFFFF;
        int stride = bb.getShort() & 0xFFFF;
        int page = bb.getInt();
        int rawSize = bb.getInt();
        int crc32 = bb.getInt();
        return new int[]{version, codec, width, height, stride, page, rawSize, crc32};
    }
}
