package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchApplier;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Random;

/** Pure-Java M1 self-checks for the logical browser framebuffer and patch chain. */
public final class BrowserPatchTest {
    public static void main(String[] args) {
        keyframeRoundtrip();
        noChangeRoundtrip();
        sparseTextLikeRoundtrip();
        mergedTileRectangle();
        fullFrameFallback();
        randomRoundtrips();
        longPatchChain();
        baseMismatchRejected();
        rgbaStrideConversion();
        System.out.println("OK: browser patch self-checks passed");
    }

    private static void keyframeRoundtrip() {
        byte[] target = LogicalMonoFrame.white();
        paintRect(target, 32, 80, 160, 48, true);
        FramePatch patch = FrameDiffer.diff(null, -1, target, 0);
        assertTrue(patch.keyframe, "first frame must be keyframe");
        assertEquals(LogicalMonoFrame.SIZE, patch.payloadBytes(), "keyframe payload size");
        assertArray(target, PatchApplier.apply(null, -1, patch), "keyframe exact reconstruction");
    }

    private static void noChangeRoundtrip() {
        byte[] base = LogicalMonoFrame.white();
        paintRect(base, 48, 112, 96, 32, true);
        FramePatch patch = FrameDiffer.diff(base, 7, Arrays.copyOf(base, base.length), 8);
        assertTrue(!patch.keyframe, "no-change is not keyframe");
        assertEquals(0, patch.changedTiles, "no-change tile count");
        assertEquals(0, patch.rects.size(), "no-change rect count");
        assertArray(base, PatchApplier.apply(base, 7, patch), "no-change reconstruction");
    }

    private static void sparseTextLikeRoundtrip() {
        byte[] base = LogicalMonoFrame.white();
        byte[] target = Arrays.copyOf(base, base.length);
        for (int line = 0; line < 5; line++) {
            int y = 96 + line * 48;
            paintRect(target, 32, y, 256, 10, true);
            paintRect(target, 320, y, 96, 10, true);
        }
        FramePatch patch = FrameDiffer.diff(base, 2, target, 3);
        assertTrue(!patch.keyframe, "sparse text should stay delta");
        assertTrue(patch.changedTiles > 0, "sparse text changed tiles");
        assertTrue(patch.dirtyRatio() < FrameDiffer.KEYFRAME_DIRTY_RATIO, "sparse dirty ratio");
        assertTrue(patch.payloadBytes() < LogicalMonoFrame.SIZE / 3, "sparse payload bounded");
        assertArray(target, PatchApplier.apply(base, 2, patch), "sparse text reconstruction");
    }

    private static void mergedTileRectangle() {
        byte[] base = LogicalMonoFrame.white();
        byte[] target = Arrays.copyOf(base, base.length);
        // One changed pixel in each tile of a 2x2 block should merge into one 32x32 rect.
        LogicalMonoFrame.setBlack(target, 64, 160, true);
        LogicalMonoFrame.setBlack(target, 80, 160, true);
        LogicalMonoFrame.setBlack(target, 64, 176, true);
        LogicalMonoFrame.setBlack(target, 80, 176, true);
        FramePatch patch = FrameDiffer.diff(base, 10, target, 11);
        assertEquals(4, patch.changedTiles, "2x2 tile count");
        assertEquals(1, patch.rects.size(), "2x2 block merges to one rect");
        assertEquals(32, patch.rects.get(0).width, "merged width");
        assertEquals(32, patch.rects.get(0).height, "merged height");
        assertArray(target, PatchApplier.apply(base, 10, patch), "merged rect reconstruction");
    }

    private static void fullFrameFallback() {
        byte[] base = LogicalMonoFrame.white();
        byte[] target = new byte[LogicalMonoFrame.SIZE]; // all black
        FramePatch patch = FrameDiffer.diff(base, 20, target, 21);
        assertTrue(patch.keyframe, "dense change becomes keyframe");
        assertEquals(LogicalMonoFrame.SIZE, patch.payloadBytes(), "dense keyframe payload");
        assertArray(target, PatchApplier.apply(base, 20, patch), "dense keyframe reconstruction");
    }

    private static void randomRoundtrips() {
        Random rnd = new Random(0x4d344231L);
        byte[] base = LogicalMonoFrame.white();
        long id = 30;
        for (int round = 0; round < 100; round++) {
            byte[] target = Arrays.copyOf(base, base.length);
            int edits = 1 + rnd.nextInt(250);
            for (int i = 0; i < edits; i++) {
                int x = rnd.nextInt(LogicalMonoFrame.WIDTH);
                int y = rnd.nextInt(LogicalMonoFrame.HEIGHT);
                LogicalMonoFrame.setBlack(target, x, y,
                        !LogicalMonoFrame.isBlack(target, x, y));
            }
            FramePatch patch = FrameDiffer.diff(base, id, target, id + 1);
            byte[] rebuilt = PatchApplier.apply(base, id, patch);
            assertArray(target, rebuilt, "random patch round " + round);
            base = rebuilt;
            id++;
        }
    }

    private static void longPatchChain() {
        Random rnd = new Random(1234567L);
        byte[] current = LogicalMonoFrame.white();
        long currentId = 1000;
        for (int frame = 0; frame < 400; frame++) {
            byte[] target = Arrays.copyOf(current, current.length);
            int tileX = rnd.nextInt(FrameDiffer.TILES_X) * FrameDiffer.TILE_WIDTH;
            int tileY = rnd.nextInt(FrameDiffer.TILES_Y) * FrameDiffer.TILE_HEIGHT;
            paintRect(target, tileX, tileY, FrameDiffer.TILE_WIDTH, 2 + rnd.nextInt(12),
                    (frame & 1) == 0);
            // Guarantee a difference even if painting white over an already-white tile.
            int px = tileX + (frame % FrameDiffer.TILE_WIDTH);
            int py = tileY + (frame % FrameDiffer.TILE_HEIGHT);
            LogicalMonoFrame.setBlack(target, px, py,
                    !LogicalMonoFrame.isBlack(current, px, py));

            FramePatch patch = FrameDiffer.diff(current, currentId, target, currentId + 1);
            assertTrue(patch.hasChanges(), "long chain frame must change");
            current = PatchApplier.apply(current, currentId, patch);
            assertArray(target, current, "long chain frame " + frame);
            currentId++;
        }
    }

    private static void baseMismatchRejected() {
        byte[] base = LogicalMonoFrame.white();
        byte[] target = Arrays.copyOf(base, base.length);
        LogicalMonoFrame.setBlack(target, 8, 8, true);
        FramePatch patch = FrameDiffer.diff(base, 50, target, 51);
        boolean rejected = false;
        try {
            PatchApplier.apply(base, 49, patch);
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        assertTrue(rejected, "wrong base frame id rejected");
    }

    private static void rgbaStrideConversion() {
        int pixelStride = 4;
        int rowStride = LogicalMonoFrame.WIDTH * pixelStride + 32;
        ByteBuffer rgba = ByteBuffer.allocate(rowStride * LogicalMonoFrame.HEIGHT);
        for (int y = 0; y < LogicalMonoFrame.HEIGHT; y++) {
            for (int x = 0; x < LogicalMonoFrame.WIDTH; x++) {
                int off = y * rowStride + x * pixelStride;
                rgba.put(off, (byte) 0xFF);
                rgba.put(off + 1, (byte) 0xFF);
                rgba.put(off + 2, (byte) 0xFF);
                rgba.put(off + 3, (byte) 0xFF);
            }
        }
        int blackX = 123;
        int blackY = 456;
        int off = blackY * rowStride + blackX * pixelStride;
        rgba.put(off, (byte) 0x00);
        rgba.put(off + 1, (byte) 0x00);
        rgba.put(off + 2, (byte) 0x00);

        byte[] mono = LogicalMonoFrame.fromRgba(rgba, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, rowStride, pixelStride, 128);
        assertTrue(LogicalMonoFrame.isBlack(mono, blackX, blackY), "RGBA black pixel");
        assertTrue(!LogicalMonoFrame.isBlack(mono, 122, 456), "RGBA neighbor stays white");
    }

    private static void paintRect(byte[] frame, int x, int y, int width, int height, boolean black) {
        for (int yy = y; yy < y + height; yy++) {
            for (int xx = x; xx < x + width; xx++) {
                LogicalMonoFrame.setBlack(frame, xx, yy, black);
            }
        }
    }

    private static void assertArray(byte[] expected, byte[] actual, String message) {
        if (!Arrays.equals(expected, actual)) throw new AssertionError(message);
    }

    private static void assertTrue(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void assertEquals(int expected, int actual, String message) {
        if (expected != actual) {
            throw new AssertionError(message + ": expected=" + expected + " actual=" + actual);
        }
    }
}
