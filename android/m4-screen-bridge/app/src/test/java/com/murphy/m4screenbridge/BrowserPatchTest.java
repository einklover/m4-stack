package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.patch.ExtraDimCompensation;
import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchApplier;
import com.murphy.m4screenbridge.browser.patch.RgbaFrameProbe;

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
        blackPixelCount();
        rgbSignatureIgnoresAlpha();
        rgbaProbeLumaAndMotorolaStride();
        highContrastStimulusStaysDelta();
        extraDimCompensationRecoversWhite();
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

    private static void blackPixelCount() {
        byte[] white = LogicalMonoFrame.white();
        assertEquals(0, LogicalMonoFrame.countBlack(white), "white frame has no black pixels");
        LogicalMonoFrame.setBlack(white, 0, 0, true);
        assertEquals(1, LogicalMonoFrame.countBlack(white), "one black pixel");
        byte[] black = new byte[LogicalMonoFrame.SIZE];
        assertEquals(LogicalMonoFrame.WIDTH * LogicalMonoFrame.HEIGHT,
                LogicalMonoFrame.countBlack(black), "all-zero plane is all black");
    }

    private static void rgbSignatureIgnoresAlpha() {
        ByteBuffer a = solidRgba(0x10, 0x20, 0x30, 0xFF, 1920, 4);
        ByteBuffer b = solidRgba(0x10, 0x20, 0x30, 0x01, 1920, 4);
        RgbaFrameProbe pa = RgbaFrameProbe.inspect(a, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, 1920, 4);
        RgbaFrameProbe pb = RgbaFrameProbe.inspect(b, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, 1920, 4);
        assertTrue(pa.rgbSignature == pb.rgbSignature, "RGB signature ignores alpha");
        assertTrue(pa.rgbaSignature != pb.rgbaSignature, "RGBA signature sees alpha");
        assertEquals(RgbaFrameProbe.luma(0x10, 0x20, 0x30), pa.lumaMin, "constant luma min");
        assertEquals(pa.lumaMin, pa.lumaMax, "constant luma max");
    }

    private static void rgbaProbeLumaAndMotorolaStride() {
        int pixelStride = 4;
        int rowStride = 3072; // Motorola XT2437-4 padded ImageReader plane
        ByteBuffer rgba = solidRgba(0xFF, 0xFF, 0xFF, 0xFF, rowStride, pixelStride);
        fillRgbaRect(rgba, rowStride, pixelStride, 0, 160, LogicalMonoFrame.WIDTH, 320,
                0x00, 0x00, 0x00, 0xFF);
        RgbaFrameProbe probe = RgbaFrameProbe.inspect(rgba, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, rowStride, pixelStride);
        assertEquals(0, probe.lumaMin, "black region luma min");
        assertEquals(255, probe.lumaMax, "white chrome luma max");
        assertTrue(probe.lumaMin < RgbaFrameProbe.LUMA_THRESHOLD
                && probe.lumaMax >= RgbaFrameProbe.LUMA_THRESHOLD, "luma straddles threshold");
        assertEquals(LogicalMonoFrame.WIDTH * 320, probe.lumaBelowThreshold,
                "dark pixel count matches 40% block");
        assertTrue(probe.meanLuma() > 100 && probe.meanLuma() < 200, "mean luma is mixed");
        int[] whitePx = RgbaFrameProbe.pixelRgba(rgba, 10, 10, rowStride, pixelStride);
        int[] blackPx = RgbaFrameProbe.pixelRgba(rgba, 240, 320, rowStride, pixelStride);
        assertEquals(255, whitePx[0], "white probe R");
        assertEquals(255, whitePx[3], "white probe A");
        assertEquals(0, blackPx[0], "black probe R");
        assertEquals(255, blackPx[3], "black probe A");

        byte[] mono = LogicalMonoFrame.fromRgba(rgba, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, rowStride, pixelStride, 128);
        assertEquals(LogicalMonoFrame.WIDTH * 320, LogicalMonoFrame.countBlack(mono),
                "mono black count matches 40% block");
    }

    private static void highContrastStimulusStaysDelta() {
        byte[] white = LogicalMonoFrame.white();
        byte[] blackBlock = LogicalMonoFrame.white();
        paintRect(blackBlock, 0, 160, LogicalMonoFrame.WIDTH, 320, true);
        FramePatch toBlack = FrameDiffer.diff(white, 0, blackBlock, 1);
        assertTrue(!toBlack.keyframe, "40% stimulus must stay a delta");
        assertTrue(toBlack.changedTiles > 0, "40% stimulus dirty tiles");
        assertTrue(toBlack.dirtyRatio() > 0.35 && toBlack.dirtyRatio() < 0.45,
                "40% stimulus dirty ratio");
        assertTrue(toBlack.dirtyRatio() < FrameDiffer.KEYFRAME_DIRTY_RATIO,
                "40% stimulus below keyframe threshold");
        assertArray(blackBlock, PatchApplier.apply(white, 0, toBlack), "black-block reconstruction");

        byte[] backToWhite = LogicalMonoFrame.white();
        FramePatch toWhite = FrameDiffer.diff(blackBlock, 1, backToWhite, 2);
        assertTrue(!toWhite.keyframe, "return flip stays a delta");
        assertEquals(toBlack.changedTiles, toWhite.changedTiles, "symmetric dirty tiles");
        assertArray(backToWhite, PatchApplier.apply(blackBlock, 1, toWhite),
                "white reconstruction");
    }

    private static void extraDimCompensationRecoversWhite() {
        assertEquals(89, ExtraDimCompensation.strengthFromSlider(99,
                ExtraDimCompensation.DEFAULT_STRENGTH_MIN,
                ExtraDimCompensation.DEFAULT_STRENGTH_MAX), "slider 99 maps to strength 89");
        float k = ExtraDimCompensation.componentValue(89,
                ExtraDimCompensation.DEFAULT_A, ExtraDimCompensation.DEFAULT_B,
                ExtraDimCompensation.DEFAULT_C);
        assertTrue(k > 0.14f && k < 0.16f, "Motorola extra-dim k at strength 89");
        assertEquals(ExtraDimCompensation.UNITY_GAIN,
                ExtraDimCompensation.inverseGain256(false, 99), "inactive extra dim is unity");
        int gain = ExtraDimCompensation.inverseGain256(true, 99);
        assertTrue(gain > 1600 && gain < 1800, "inverse gain for slider 99");
        assertTrue(ExtraDimCompensation.applyGain(38, gain) >= 250, "38 recovers near white");
        assertEquals(0, ExtraDimCompensation.applyGain(0, gain), "black stays black");
        int auto = ExtraDimCompensation.autoGain256(37, 128);
        assertTrue(auto > ExtraDimCompensation.UNITY_GAIN, "auto gain when max luma < threshold");
        assertTrue(ExtraDimCompensation.applyGain(38, auto) >= 250, "auto gain recovers 38");
        assertEquals(ExtraDimCompensation.UNITY_GAIN,
                ExtraDimCompensation.autoGain256(247, 128), "no auto gain when white already crosses");

        int pixelStride = 4;
        int rowStride = 3072;
        ByteBuffer rgba = solidRgba(38, 37, 35, 255, rowStride, pixelStride);
        fillRgbaRect(rgba, rowStride, pixelStride, 0, 160, LogicalMonoFrame.WIDTH, 320,
                0, 0, 0, 255);
        byte[] without = LogicalMonoFrame.fromRgba(rgba, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, rowStride, pixelStride, 128);
        assertEquals(LogicalMonoFrame.WIDTH * LogicalMonoFrame.HEIGHT,
                LogicalMonoFrame.countBlack(without), "uncompensated extra-dim is all black");
        byte[] with = LogicalMonoFrame.fromRgba(rgba, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, rowStride, pixelStride, 128, gain);
        assertEquals(LogicalMonoFrame.WIDTH * 320, LogicalMonoFrame.countBlack(with),
                "compensated extra-dim keeps only the 40% block black");
        FramePatch patch = FrameDiffer.diff(LogicalMonoFrame.white(), 0, with, 1);
        assertTrue(!patch.keyframe, "compensated 40% stimulus stays a delta");
        assertTrue(patch.dirtyRatio() > 0.35 && patch.dirtyRatio() < 0.45,
                "compensated dirty ratio");
    }

    private static ByteBuffer solidRgba(int r, int g, int b, int a, int rowStride, int pixelStride) {
        ByteBuffer rgba = ByteBuffer.allocate(rowStride * LogicalMonoFrame.HEIGHT);
        fillRgbaRect(rgba, rowStride, pixelStride, 0, 0, LogicalMonoFrame.WIDTH,
                LogicalMonoFrame.HEIGHT, r, g, b, a);
        return rgba;
    }

    private static void fillRgbaRect(ByteBuffer rgba, int rowStride, int pixelStride,
            int x0, int y0, int width, int height, int r, int g, int b, int a) {
        for (int y = y0; y < y0 + height; y++) {
            for (int x = x0; x < x0 + width; x++) {
                int off = y * rowStride + x * pixelStride;
                rgba.put(off, (byte) r);
                rgba.put(off + 1, (byte) g);
                rgba.put(off + 2, (byte) b);
                if (pixelStride >= 4) rgba.put(off + 3, (byte) a);
            }
        }
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
