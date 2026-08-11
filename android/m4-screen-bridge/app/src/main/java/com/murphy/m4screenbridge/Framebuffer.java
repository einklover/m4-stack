package com.murphy.m4screenbridge;

import java.util.Arrays;

/** M4 physical framebuffer packing. Pure Java (testable off-device).
 *
 * Logical portrait: x 0..479, y 0..799. Physical: phyX = y, phyY = 479 - x.
 * byte = phyY*100 + phyX/8, bit = 7 - (phyX % 8). White initializes 1, black clears 0. */
public final class Framebuffer {
    public static final int PHYS_W = 800;
    public static final int PHYS_H = 480;
    public static final int STRIDE = 100;
    public static final int RAW_SIZE = PHYS_W * PHYS_H / 8; // 48000
    public static final int LOGICAL_W = 480;
    public static final int LOGICAL_H = 800;

    private Framebuffer() {}

    /** Pack a logical portrait page (page[y][x], true = black) into raw framebuffer bytes. */
    public static byte[] pack(boolean[][] page) {
        byte[] fb = new byte[RAW_SIZE];
        Arrays.fill(fb, (byte) 0xFF);
        for (int y = 0; y < LOGICAL_H; y++) {
            boolean[] row = page[y];
            for (int x = 0; x < LOGICAL_W; x++) {
                if (!row[x]) continue;
                int phyX = y;
                int phyY = LOGICAL_W - 1 - x;
                int idx = phyY * STRIDE + (phyX >> 3);
                int bit = 7 - (phyX & 7);
                fb[idx] &= (byte) ~(1 << bit);
            }
        }
        return fb;
    }

    /** Inverse of pack: is logical pixel (x, y) black? */
    public static boolean logicalPixel(byte[] fb, int x, int y) {
        int phyX = y;
        int phyY = LOGICAL_W - 1 - x;
        int idx = phyY * STRIDE + (phyX >> 3);
        int bit = 7 - (phyX & 7);
        return (fb[idx] & (1 << bit)) == 0;
    }

}
