package com.murphy.m4screenbridge.browser.patch;

import java.util.Arrays;

/** Reference patch applier used by tests now and by the future receiver contract. */
public final class PatchApplier {
    private PatchApplier() {}

    public static byte[] apply(byte[] base, long currentFrameId, FramePatch patch) {
        if (patch == null) throw new IllegalArgumentException("patch is null");
        byte[] out;
        if (patch.keyframe) {
            out = LogicalMonoFrame.white();
        } else {
            LogicalMonoFrame.validate(base);
            if (currentFrameId != patch.baseFrameId) {
                throw new IllegalArgumentException("base frame mismatch: have " + currentFrameId
                        + ", patch needs " + patch.baseFrameId);
            }
            out = Arrays.copyOf(base, base.length);
        }

        for (PatchRect rect : patch.rects) {
            int dstX = rect.x / 8;
            int rectStride = rect.stride();
            for (int row = 0; row < rect.height; row++) {
                int dst = (rect.y + row) * LogicalMonoFrame.STRIDE + dstX;
                System.arraycopy(rect.data, row * rectStride, out, dst, rectStride);
            }
        }
        return out;
    }
}
