package com.murphy.m4screenbridge.browser.patch;

import java.nio.ByteBuffer;
import java.util.Arrays;

/** Logical portrait 480x800 1bpp framebuffer used by the Browser Bridge protocol core. */
public final class LogicalMonoFrame {
    public static final int WIDTH = 480;
    public static final int HEIGHT = 800;
    public static final int STRIDE = WIDTH / 8;
    public static final int SIZE = STRIDE * HEIGHT;

    private LogicalMonoFrame() {}

    public static byte[] white() {
        byte[] frame = new byte[SIZE];
        Arrays.fill(frame, (byte) 0xFF);
        return frame;
    }

    public static void validate(byte[] frame) {
        if (frame == null || frame.length != SIZE) {
            throw new IllegalArgumentException("logical frame must be exactly " + SIZE + " bytes");
        }
    }

    public static boolean isBlack(byte[] frame, int x, int y) {
        validate(frame);
        checkPixel(x, y);
        int off = y * STRIDE + (x >>> 3);
        int mask = 0x80 >>> (x & 7);
        return (frame[off] & mask) == 0;
    }

    public static void setBlack(byte[] frame, int x, int y, boolean black) {
        validate(frame);
        checkPixel(x, y);
        int off = y * STRIDE + (x >>> 3);
        int mask = 0x80 >>> (x & 7);
        if (black) frame[off] = (byte) (frame[off] & ~mask);
        else frame[off] = (byte) (frame[off] | mask);
    }

    /**
     * Converts an RGBA_8888 ImageReader plane into the stable logical 1bpp frame.
     * White is bit 1 and black is bit 0, matching the existing M4 framebuffer convention.
     */
    public static byte[] fromRgba(ByteBuffer source, int width, int height,
            int rowStride, int pixelStride, int threshold) {
        if (source == null) throw new IllegalArgumentException("source is null");
        if (width != WIDTH || height != HEIGHT) {
            throw new IllegalArgumentException("expected " + WIDTH + "x" + HEIGHT
                    + " RGBA frame, got " + width + "x" + height);
        }
        if (pixelStride < 3 || rowStride < width * pixelStride) {
            throw new IllegalArgumentException("invalid RGBA strides");
        }
        if (threshold < 0 || threshold > 255) {
            throw new IllegalArgumentException("threshold out of range");
        }

        ByteBuffer b = source.duplicate();
        byte[] out = white();
        for (int y = 0; y < HEIGHT; y++) {
            int row = y * rowStride;
            for (int x = 0; x < WIDTH; x++) {
                int off = row + x * pixelStride;
                if (off < 0 || off + 2 >= b.limit()) {
                    throw new IllegalArgumentException("RGBA plane shorter than declared strides");
                }
                int r = b.get(off) & 0xFF;
                int g = b.get(off + 1) & 0xFF;
                int bl = b.get(off + 2) & 0xFF;
                int luma = (77 * r + 150 * g + 29 * bl) >>> 8;
                if (luma < threshold) {
                    int dst = y * STRIDE + (x >>> 3);
                    out[dst] = (byte) (out[dst] & ~(0x80 >>> (x & 7)));
                }
            }
        }
        return out;
    }

    private static void checkPixel(int x, int y) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
            throw new IllegalArgumentException("pixel out of bounds");
        }
    }
}
