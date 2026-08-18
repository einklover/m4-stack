package com.murphy.m4screenbridge.browser.patch;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Pure-Java patch model. Transport encoding is deliberately deferred to M2. */
public final class FramePatch {
    public final long frameId;
    public final long baseFrameId;
    public final boolean keyframe;
    public final int changedTiles;
    public final int totalTiles;
    public final List<PatchRect> rects;

    public FramePatch(long frameId, long baseFrameId, boolean keyframe,
            int changedTiles, int totalTiles, List<PatchRect> rects) {
        if (frameId < 0) throw new IllegalArgumentException("frameId must be non-negative");
        if (!keyframe && baseFrameId < 0) {
            throw new IllegalArgumentException("delta patch requires a base frame id");
        }
        if (changedTiles < 0 || totalTiles <= 0 || changedTiles > totalTiles) {
            throw new IllegalArgumentException("invalid tile counts");
        }
        if (rects == null) throw new IllegalArgumentException("rects is null");
        this.frameId = frameId;
        this.baseFrameId = keyframe ? -1 : baseFrameId;
        this.keyframe = keyframe;
        this.changedTiles = changedTiles;
        this.totalTiles = totalTiles;
        this.rects = Collections.unmodifiableList(new ArrayList<>(rects));
    }

    public int payloadBytes() {
        int total = 0;
        for (PatchRect rect : rects) total += rect.payloadBytes();
        return total;
    }

    public double dirtyRatio() {
        return changedTiles / (double) totalTiles;
    }

    public boolean hasChanges() {
        return keyframe || !rects.isEmpty();
    }
}
