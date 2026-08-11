package com.murphy.m4screenbridge;

/** Screen -> logical 480x800 monochrome preprocessing. Pure Java (testable off-device). */
public final class Preprocess {
    public static final int W = Framebuffer.LOGICAL_W;
    public static final int H = Framebuffer.LOGICAL_H;

    private Preprocess() {}

    /** Conservative classifier used to leave cached auto-turn mode on non-text screens. */
    public static boolean looksLikeTextPage(int[] gray, int sourceH, int threshold) {
        int top = sourceH / 16;
        int bottom = sourceH - sourceH / 16;
        long light = 0;
        long dark = 0;
        long total = 0;
        int bands = 0;
        int wideBands = 0;
        int bandStart = -1;
        int lastInk = -1;
        int bandMinX = W;
        int bandMaxX = -1;
        int denseRows = 0;
        for (int y = top; y < bottom; y++) {
            int rowDark = 0;
            int firstInk = W;
            int lastRowInk = -1;
            int base = y * W;
            for (int x = W / 30; x < W - W / 30; x++) {
                int v = gray[base + x] & 0xFF;
                if (v > 220) light++;
                if (v < threshold) {
                    dark++;
                    rowDark++;
                    firstInk = Math.min(firstInk, x);
                    lastRowInk = x;
                }
                total++;
            }
            boolean ink = rowDark >= 4;
            if (rowDark > W * 3 / 5) denseRows++;
            if (ink) {
                if (bandStart < 0) bandStart = y;
                lastInk = y;
                bandMinX = Math.min(bandMinX, firstInk);
                bandMaxX = Math.max(bandMaxX, lastRowInk);
            } else if (bandStart >= 0 && y - lastInk > 8) {
                bands++;
                if (bandMaxX - bandMinX + 1 >= W / 2) wideBands++;
                bandStart = -1;
                bandMinX = W;
                bandMaxX = -1;
            }
        }
        if (bandStart >= 0) {
            bands++;
            if (bandMaxX - bandMinX + 1 >= W / 2) wideBands++;
        }
        return light * 100 >= total * 68
                && dark * 100 <= total * 30
                && denseRows < Math.max(4, sourceH / 20)
                && (bands >= 4 || (bands >= 2 && wideBands >= 1));
    }

    /**
     * Reflows one width-fitted phone page without altering glyph proportions.
     * Only blank space between detected text rows is changed. If the rows plus
     * the rows plus their capped gaps do not fit, every row is scaled equally in X
     * and Y, then centered with a safe top/bottom margin.
     */
    public static int[] autoLayout(int[] gray, int sourceH, int threshold, int maxGap) {
        java.util.ArrayList<int[]> lines = new java.util.ArrayList<>();
        int topCut = sourceH / 16;
        int bottomCut = sourceH - sourceH / 16;
        int start = -1;
        int lastInk = -1;
        for (int y = topCut; y < bottomCut; y++) {
            int dark = 0;
            int base = y * W;
            for (int x = W / 30; x < W - W / 30; x++) {
                if ((gray[base + x] & 0xFF) < threshold && ++dark >= 4) break;
            }
            if (dark >= 4) {
                if (start < 0) start = y;
                lastInk = y;
            } else if (start >= 0 && y - lastInk > 8) {
                lines.add(new int[]{Math.max(topCut, start - 2), Math.min(bottomCut, lastInk + 3)});
                start = -1;
            }
        }
        if (start >= 0) {
            lines.add(new int[]{Math.max(topCut, start - 2), Math.min(bottomCut, lastInk + 3)});
        }

        // Fanqie draws small toolbar/footer labels around the much larger body
        // text. Drop only leading/trailing bands that are clearly a smaller font.
        int maxLineH = 0;
        for (int[] line : lines) maxLineH = Math.max(maxLineH, line[1] - line[0]);
        while (!lines.isEmpty() && (lines.get(0)[1] - lines.get(0)[0]) * 2 < maxLineH) {
            lines.remove(0);
        }
        while (!lines.isEmpty() && (lines.get(lines.size() - 1)[1]
                - lines.get(lines.size() - 1)[0]) * 2 < maxLineH) {
            lines.remove(lines.size() - 1);
        }

        int[] out = new int[W * H];
        java.util.Arrays.fill(out, 255);
        if (lines.isEmpty()) return out;

        final int margin = 24;
        final int available = H - margin * 2;
        int glyphRows = 0;
        for (int[] line : lines) glyphRows += line[1] - line[0];
        int gaps = Math.max(0, lines.size() - 1);
        int gapLimit = Math.max(0, Math.min(80, maxGap));
        int naturalGap = gapLimit;
        if (gaps > 0) {
            int[] measured = new int[gaps];
            for (int i = 0; i < gaps; i++) {
                measured[i] = Math.max(0, lines.get(i + 1)[0] - lines.get(i)[1]);
            }
            java.util.Arrays.sort(measured);
            naturalGap = measured[measured.length / 2];
        }
        int preferredGap = Math.min(gapLimit, naturalGap);
        int minRows = glyphRows + gaps * preferredGap;
        double scale = minRows > available ? (double) available / minRows : 1.0;

        int scaledGlyphRows = 0;
        for (int[] line : lines) {
            scaledGlyphRows += Math.max(1, (int) Math.round((line[1] - line[0]) * scale));
        }
        int scaledPreferredGap = gaps == 0 ? 0
                : Math.max(0, (int) Math.round(preferredGap * scale));
        int fitGap = gaps == 0 ? 0 : Math.max(0, (available - scaledGlyphRows) / gaps);
        int gap = gaps == 0 ? 0 : Math.min(fitGap, scaledPreferredGap);
        int contentH = scaledGlyphRows + gaps * gap;
        int dstY = Math.max(margin, (H - contentH) / 2);
        int dstW = Math.max(1, Math.min(W, (int) Math.round(W * scale)));
        int dstX = (W - dstW) / 2;

        for (int li = 0; li < lines.size(); li++) {
            int[] line = lines.get(li);
            int srcRows = line[1] - line[0];
            int dstRows = Math.max(1, (int) Math.round(srcRows * scale));
            for (int dy = 0; dy < dstRows && dstY + dy < H - margin; dy++) {
                int srcY = line[0] + Math.min(srcRows - 1, dy * srcRows / dstRows);
                int outBase = (dstY + dy) * W + dstX;
                int srcBase = srcY * W;
                for (int dx = 0; dx < dstW; dx++) {
                    int srcX = Math.min(W - 1, dx * W / dstW);
                    out[outBase + dx] = gray[srcBase + srcX];
                }
            }
            dstY += dstRows + gap;
        }
        return out;
    }

    /**
     * Collapses blank vertical gaps before dithering. A row needs several dark
     * pixels in the central area to count as ink, so isolated antialiasing or
     * screenshot noise does not prevent gap compression.
     */
    public static int[] compressGrayGaps(int[] gray, int threshold, int maxGap) {
        boolean[] ink = new boolean[H];
        int x0 = W / 20;
        int x1 = W - x0;
        for (int y = 0; y < H; y++) {
            int dark = 0;
            int base = y * W;
            for (int x = x0; x < x1; x++) {
                if ((gray[base + x] & 0xFF) < threshold && ++dark >= 4) {
                    ink[y] = true;
                    break;
                }
            }
        }

        int[] out = new int[W * H];
        java.util.Arrays.fill(out, 255);
        int srcY = 0;
        int dstY = 0;
        int gapLimit = Math.max(0, Math.min(H, maxGap));
        while (srcY < H && dstY < H) {
            if (ink[srcY]) {
                System.arraycopy(gray, srcY * W, out, dstY * W, W);
                srcY++;
                dstY++;
                continue;
            }
            int gapStart = srcY;
            while (srcY < H && !ink[srcY]) srcY++;
            int keep = Math.min(srcY - gapStart, gapLimit);
            for (int i = 0; i < keep && dstY < H; i++, dstY++) {
                System.arraycopy(gray, (gapStart + i) * W, out, dstY * W, W);
            }
        }
        return out;
    }

    /** gray[i] (0..255) -> page[y][x], true = black, false = white. */
    public static boolean[][] threshold(int[] gray, int threshold) {
        boolean[][] m = new boolean[H][W];
        for (int y = 0; y < H; y++) {
            boolean[] row = m[y];
            int base = y * W;
            for (int x = 0; x < W; x++) {
                row[x] = (gray[base + x] & 0xFF) < threshold;
            }
        }
        return m;
    }

    /** Floyd-Steinberg error diffusion against the threshold. */
    public static boolean[][] dither(int[] gray, int threshold) {
        int[] err = new int[W * H];
        boolean[][] m = new boolean[H][W];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                int v = (gray[i] & 0xFF) + err[i];
                boolean black = v < threshold;
                m[y][x] = black;
                int q = v - (black ? 0 : 255);
                if (x + 1 < W) err[i + 1] += q * 7 / 16;
                if (y + 1 < H) {
                    if (x > 0) err[i + W - 1] += q * 3 / 16;
                    err[i + W] += q * 5 / 16;
                    if (x + 1 < W) err[i + W + 1] += q * 1 / 16;
                }
            }
        }
        return m;
    }

    /** Collapses blank vertical gaps to at most maxGap rows, always returns 800 rows. */
    public static boolean[][] compressGaps(boolean[][] mono, int maxGap) {
        boolean[] ink = new boolean[H];
        for (int y = 0; y < H; y++) {
            boolean[] row = mono[y];
            boolean any = false;
            for (int x = 0; x < W; x++) {
                if (row[x]) { any = true; break; }
            }
            ink[y] = any;
        }
        int outRows = 0;
        int y = 0;
        while (y < H) {
            if (ink[y]) { outRows++; y++; }
            else {
                int gap = 0;
                while (y < H && !ink[y]) { gap++; y++; }
                outRows += Math.min(gap, maxGap);
            }
        }
        boolean[][] rows = new boolean[Math.max(1, outRows)][W];
        y = 0;
        int o = 0;
        while (y < H) {
            if (ink[y]) {
                System.arraycopy(mono[y], 0, rows[o++], 0, W);
                y++;
            } else {
                int gap = 0;
                while (y < H && !ink[y]) { gap++; y++; }
                o += Math.min(gap, maxGap);
            }
        }
        if (outRows <= H) return pad(rows, H);
        return subsample(rows, H);
    }

    static boolean[][] pad(boolean[][] rows, int target) {
        boolean[][] r = new boolean[target][W];
        for (int i = 0; i < rows.length; i++) r[i] = rows[i];
        return r;
    }

    static boolean[][] subsample(boolean[][] rows, int target) {
        boolean[][] r = new boolean[target][W];
        for (int i = 0; i < target; i++) {
            r[i] = rows[Math.min(rows.length - 1, i * rows.length / target)];
        }
        return r;
    }
}
