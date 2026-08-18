package com.murphy.m4screenbridge.browser.patch;

import java.nio.ByteBuffer;

/**
 * RGBA plane probes used to distinguish compositor/alpha noise from real RGB/mono changes.
 * Signatures stay sampled; luma min/max/mean walk the declared frame because 480x800 is cheap.
 */
public final class RgbaFrameProbe {
    public static final int LUMA_THRESHOLD = 128;

    public final long rgbSignature;
    public final long rgbaSignature;
    public final int lumaMin;
    public final int lumaMax;
    public final long lumaSum;
    public final int sampleCount;
    public final int lumaBelowThreshold;

    public RgbaFrameProbe(long rgbSignature, long rgbaSignature, int lumaMin, int lumaMax,
            long lumaSum, int sampleCount, int lumaBelowThreshold) {
        this.rgbSignature = rgbSignature;
        this.rgbaSignature = rgbaSignature;
        this.lumaMin = lumaMin;
        this.lumaMax = lumaMax;
        this.lumaSum = lumaSum;
        this.sampleCount = sampleCount;
        this.lumaBelowThreshold = lumaBelowThreshold;
    }

    public int meanLuma() {
        return sampleCount == 0 ? 0 : (int) (lumaSum / sampleCount);
    }

    public static int luma(int r, int g, int b) {
        return (77 * r + 150 * g + 29 * b) >>> 8;
    }

    /**
     * Returns {r,g,b,a} at one pixel. {@code a} is -1 when the plane has no alpha channel
     * or the sample is out of range.
     */
    public static int[] pixelRgba(ByteBuffer source, int x, int y, int rowStride, int pixelStride) {
        if (source == null || x < 0 || y < 0 || pixelStride < 3 || rowStride <= 0) {
            return new int[] {0, 0, 0, -1};
        }
        ByteBuffer b = source.duplicate();
        int off = y * rowStride + x * pixelStride;
        if (off < 0 || off + 2 >= b.limit()) return new int[] {0, 0, 0, -1};
        int r = b.get(off) & 0xFF;
        int g = b.get(off + 1) & 0xFF;
        int bl = b.get(off + 2) & 0xFF;
        int a = (pixelStride >= 4 && off + 3 < b.limit()) ? (b.get(off + 3) & 0xFF) : -1;
        return new int[] {r, g, bl, a};
    }

    public static String formatPixel(int x, int y, int[] rgba) {
        if (rgba == null || rgba.length < 4) return x + "," + y + "=rgba(?,?,?,?)";
        return x + "," + y + "=rgba(" + rgba[0] + "," + rgba[1] + "," + rgba[2] + "," + rgba[3] + ")";
    }

    public static RgbaFrameProbe inspect(ByteBuffer source, int width, int height,
            int rowStride, int pixelStride) {
        if (source == null) throw new IllegalArgumentException("source is null");
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("invalid size");
        if (pixelStride < 3 || rowStride < width * pixelStride) {
            throw new IllegalArgumentException("invalid RGBA strides");
        }

        ByteBuffer b = source.duplicate();
        long rgbHash = 0xcbf29ce484222325L;
        long rgbaHash = 0xcbf29ce484222325L;
        int yStep = Math.max(1, height / 50);
        int xStep = Math.max(1, width / 30);
        for (int y = 0; y < height; y += yStep) {
            int row = y * rowStride;
            for (int x = 0; x < width; x += xStep) {
                int offset = row + x * pixelStride;
                if (offset < 0 || offset + 2 >= b.limit()) continue;
                for (int c = 0; c < 3; c++) {
                    long v = b.get(offset + c) & 0xffL;
                    rgbHash ^= v;
                    rgbHash *= 0x100000001b3L;
                    rgbaHash ^= v;
                    rgbaHash *= 0x100000001b3L;
                }
                if (pixelStride >= 4 && offset + 3 < b.limit()) {
                    rgbaHash ^= b.get(offset + 3) & 0xffL;
                    rgbaHash *= 0x100000001b3L;
                }
            }
        }

        int lumaMin = 255;
        int lumaMax = 0;
        long lumaSum = 0;
        int samples = 0;
        int below = 0;
        for (int y = 0; y < height; y++) {
            int row = y * rowStride;
            for (int x = 0; x < width; x++) {
                int offset = row + x * pixelStride;
                if (offset < 0 || offset + 2 >= b.limit()) {
                    throw new IllegalArgumentException("RGBA plane shorter than declared strides");
                }
                int luma = luma(b.get(offset) & 0xFF, b.get(offset + 1) & 0xFF,
                        b.get(offset + 2) & 0xFF);
                if (luma < lumaMin) lumaMin = luma;
                if (luma > lumaMax) lumaMax = luma;
                lumaSum += luma;
                samples++;
                if (luma < LUMA_THRESHOLD) below++;
            }
        }
        return new RgbaFrameProbe(rgbHash, rgbaHash, lumaMin, lumaMax, lumaSum, samples, below);
    }
}
