package com.murphy.m4screenbridge.browser.patch;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** 16x16 dirty-tile detector with deterministic rectangle merging. */
public final class FrameDiffer {
    public static final int TILE_WIDTH = 16;
    public static final int TILE_HEIGHT = 16;
    public static final int TILES_X = LogicalMonoFrame.WIDTH / TILE_WIDTH;
    public static final int TILES_Y = LogicalMonoFrame.HEIGHT / TILE_HEIGHT;
    public static final int TOTAL_TILES = TILES_X * TILES_Y;
    public static final double KEYFRAME_DIRTY_RATIO = 0.60;

    private FrameDiffer() {}

    public static FramePatch diff(byte[] base, long baseFrameId, byte[] target, long frameId) {
        LogicalMonoFrame.validate(target);
        if (base == null) return keyframe(target, frameId, TOTAL_TILES);
        LogicalMonoFrame.validate(base);
        if (baseFrameId < 0) throw new IllegalArgumentException("base frame id must be non-negative");

        boolean[][] changed = new boolean[TILES_Y][TILES_X];
        int changedTiles = 0;
        for (int ty = 0; ty < TILES_Y; ty++) {
            for (int tx = 0; tx < TILES_X; tx++) {
                if (tileChanged(base, target, tx, ty)) {
                    changed[ty][tx] = true;
                    changedTiles++;
                }
            }
        }

        if (changedTiles == 0) {
            return new FramePatch(frameId, baseFrameId, false, 0, TOTAL_TILES,
                    Collections.emptyList());
        }
        if (changedTiles / (double) TOTAL_TILES >= KEYFRAME_DIRTY_RATIO) {
            return keyframe(target, frameId, changedTiles);
        }

        List<PatchRect> rects = mergeAndExtract(changed, target);
        int bytes = 0;
        for (PatchRect rect : rects) bytes += rect.payloadBytes();
        if (bytes >= LogicalMonoFrame.SIZE) {
            return keyframe(target, frameId, changedTiles);
        }
        return new FramePatch(frameId, baseFrameId, false, changedTiles, TOTAL_TILES, rects);
    }

    private static FramePatch keyframe(byte[] target, long frameId, int changedTiles) {
        List<PatchRect> rects = Collections.singletonList(new PatchRect(0, 0,
                LogicalMonoFrame.WIDTH, LogicalMonoFrame.HEIGHT, target));
        return new FramePatch(frameId, -1, true, changedTiles, TOTAL_TILES, rects);
    }

    private static boolean tileChanged(byte[] a, byte[] b, int tx, int ty) {
        int byteX = tx * (TILE_WIDTH / 8);
        int y0 = ty * TILE_HEIGHT;
        int tileBytes = TILE_WIDTH / 8;
        for (int row = 0; row < TILE_HEIGHT; row++) {
            int off = (y0 + row) * LogicalMonoFrame.STRIDE + byteX;
            for (int i = 0; i < tileBytes; i++) {
                if (a[off + i] != b[off + i]) return true;
            }
        }
        return false;
    }

    /**
     * Greedy merge: take the maximal horizontal run, then extend it vertically while every
     * tile in that same span is still dirty. Exact optimal rectangle cover is unnecessary;
     * deterministic bounded rectangles are preferable for the ESP32 receiver.
     */
    private static List<PatchRect> mergeAndExtract(boolean[][] changed, byte[] target) {
        boolean[][] used = new boolean[TILES_Y][TILES_X];
        List<PatchRect> out = new ArrayList<>();
        for (int ty = 0; ty < TILES_Y; ty++) {
            for (int tx = 0; tx < TILES_X; tx++) {
                if (!changed[ty][tx] || used[ty][tx]) continue;

                int runEnd = tx;
                while (runEnd + 1 < TILES_X
                        && changed[ty][runEnd + 1] && !used[ty][runEnd + 1]) {
                    runEnd++;
                }

                int endY = ty;
                outer:
                while (endY + 1 < TILES_Y) {
                    for (int x = tx; x <= runEnd; x++) {
                        if (!changed[endY + 1][x] || used[endY + 1][x]) break outer;
                    }
                    endY++;
                }

                for (int y = ty; y <= endY; y++) {
                    for (int x = tx; x <= runEnd; x++) used[y][x] = true;
                }

                int x = tx * TILE_WIDTH;
                int y = ty * TILE_HEIGHT;
                int width = (runEnd - tx + 1) * TILE_WIDTH;
                int height = (endY - ty + 1) * TILE_HEIGHT;
                out.add(extract(target, x, y, width, height));
            }
        }
        return out;
    }

    private static PatchRect extract(byte[] target, int x, int y, int width, int height) {
        int rectStride = width / 8;
        byte[] data = new byte[rectStride * height];
        int srcX = x / 8;
        for (int row = 0; row < height; row++) {
            int src = (y + row) * LogicalMonoFrame.STRIDE + srcX;
            System.arraycopy(target, src, data, row * rectStride, rectStride);
        }
        return new PatchRect(x, y, width, height, data);
    }
}
