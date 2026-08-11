package com.murphy.m4screenbridge;

import java.io.ByteArrayOutputStream;
import java.util.Arrays;

/** M4 RLE1 codec (codec id 1). Pure Java.
 *
 * Token with high bit 1: run length = low7 + 3, followed by one repeated byte.
 * Token with high bit 0: literal length = low7 + 1, followed by that many bytes. */
public final class Rle {
    public static final int CODEC_RAW = 0;
    public static final int CODEC_RLE1 = 1;

    private Rle() {}

    public static byte[] encode(byte[] data) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(Math.min(data.length, 512));
        int i = 0, n = data.length;
        while (i < n) {
            byte b = data[i];
            int run = 1;
            while (i + run < n && data[i + run] == b && run < 130) run++;
            if (run >= 3) {
                out.write(0x80 | (run - 3));
                out.write(b);
                i += run;
            } else {
                int start = i;
                while (i < n && (i - start) < 128) {
                    byte c = data[i];
                    int r = 1;
                    while (i + r < n && data[i + r] == c && r < 130) r++;
                    if (r >= 3) break;
                    i++;
                }
                int len = i - start;
                out.write(len - 1);
                out.write(data, start, len);
            }
        }
        return out.toByteArray();
    }

    public static byte[] decode(byte[] enc, int rawSize) {
        byte[] out = new byte[rawSize];
        int o = 0, i = 0;
        while (i < enc.length && o < rawSize) {
            int token = enc[i++] & 0xFF;
            if ((token & 0x80) != 0) {
                int len = (token & 0x7F) + 3;
                byte b = enc[i++];
                Arrays.fill(out, o, Math.min(o + len, rawSize), b);
                o += len;
            } else {
                int len = (token & 0x7F) + 1;
                int copy = Math.min(len, rawSize - o);
                System.arraycopy(enc, i, out, o, copy);
                i += len;
                o += len;
            }
        }
        return out;
    }

    /** Use RLE only if the encoded payload is smaller than raw. */
    public static int codecFor(byte[] raw, byte[] encoded) {
        return encoded.length < raw.length ? CODEC_RLE1 : CODEC_RAW;
    }
}
