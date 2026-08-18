package com.murphy.m4screenbridge.browser.patch;

import java.util.Arrays;

/** One byte-aligned logical framebuffer rectangle and its packed 1bpp payload. */
public final class PatchRect {
    public final int x;
    public final int y;
    public final int width;
    public final int height;
    public final byte[] data;

    public PatchRect(int x, int y, int width, int height, byte[] data) {
        if (x < 0 || y < 0 || width <= 0 || height <= 0
                || x + width > LogicalMonoFrame.WIDTH
                || y + height > LogicalMonoFrame.HEIGHT) {
            throw new IllegalArgumentException("patch rectangle out of bounds");
        }
        if ((x & 7) != 0 || (width & 7) != 0) {
            throw new IllegalArgumentException("patch x/width must be byte aligned");
        }
        int expected = (width >>> 3) * height;
        if (data == null || data.length != expected) {
            throw new IllegalArgumentException("patch payload must be " + expected + " bytes");
        }
        this.x = x;
        this.y = y;
        this.width = width;
        this.height = height;
        this.data = Arrays.copyOf(data, data.length);
    }

    public int stride() {
        return width >>> 3;
    }

    public int payloadBytes() {
        return data.length;
    }
}
