package com.murphy.m4screenbridge.browser.stream;

import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.patch.PatchRect;

import java.util.Arrays;
import java.util.Collections;

/**
 * ACK-based one-frame-in-flight sender. The last ACKed framebuffer is the only
 * valid diff base. Captures that arrive while a frame is outstanding replace a
 * single pending snapshot (latest-frame-wins) and never queue.
 *
 * Encoding happens here; the outbound sink must not block on a socket.
 */
public final class M4B3Sender {
    private final M4B3Outbound outbound;

    private boolean helloOk;
    private boolean forceKeyframe = true;
    private boolean inFlight;
    private long nextFrameId;
    private long nextSeq;
    private long ackedFrameId = -1;
    private long inFlightFrameId = -1;
    private int ackedCrc;
    private int inFlightCrc;
    private byte[] ackedFrame;
    private byte[] inFlightFrame;
    private byte[] pendingNewest;
    private byte[] lastOffered;

    private long keyframesSent;
    private long patchesSent;
    private long unchangedSuppressed;
    private long nackRecoveries;
    private long latestWinsDrops;
    private long hellosSent;
    private long payloadBytesSent;
    private int lastChangedTiles;
    private int lastRectCount;
    private int lastPatchBytes;
    private double lastDirtyRatio;

    public M4B3Sender(M4B3Outbound outbound) {
        if (outbound == null) throw new IllegalArgumentException("outbound is null");
        this.outbound = outbound;
    }

    public synchronized void connect() {
        beginSession(true);
    }

    public synchronized void reconnect() {
        beginSession(true);
    }

    /** Drop HELLO confidence without forgetting the newest snapshot. */
    public synchronized void noteTransportLost() {
        helloOk = false;
        forceKeyframe = true;
        clearInFlight();
    }

    public synchronized void disconnect() {
        resetSession(false);
    }

    /**
     * Offer the newest logical MONO1 framebuffer. Must not perform socket I/O.
     * The caller may reuse {@code frame} after this returns.
     */
    public synchronized void offerFrame(byte[] frame) {
        LogicalMonoFrame.validate(frame);
        byte[] owned = Arrays.copyOf(frame, frame.length);
        lastOffered = owned;
        if (!helloOk || inFlight) {
            if (inFlight && pendingNewest != null) latestWinsDrops++;
            pendingNewest = owned;
            return;
        }
        trySend(owned);
    }

    public synchronized void receive(byte[] packet) {
        M4B3Message msg = M4B3Codec.parse(packet);
        switch (msg.type) {
            case M4B3.TYPE_HELLO:
                onHello(msg.hello);
                break;
            case M4B3.TYPE_FRAME_ACK:
                onAck(msg.ack);
                break;
            case M4B3.TYPE_PING:
                emit(M4B3Codec.encodePong(msg.nonce, nextSeq++));
                break;
            case M4B3.TYPE_PONG:
            case M4B3.TYPE_TOUCH:
                break;
            default:
                throw M4B3Exception.invalid("sender rejected unexpected " + M4B3.typeName(msg.type));
        }
    }

    public synchronized void ping(long nonce) {
        emit(M4B3Codec.encodePing(nonce, nextSeq++));
    }

    public synchronized Stats stats() {
        return new Stats(
                helloOk, forceKeyframe, inFlight, hasPending(),
                ackedFrameId, inFlightFrameId, nextFrameId,
                ackedCrc, inFlightCrc,
                keyframesSent, patchesSent, unchangedSuppressed,
                nackRecoveries, latestWinsDrops, hellosSent, payloadBytesSent,
                lastChangedTiles, lastRectCount, lastPatchBytes, lastDirtyRatio);
    }

    public synchronized byte[] copyAckedFramebuffer() {
        return ackedFrame == null ? null : Arrays.copyOf(ackedFrame, ackedFrame.length);
    }

    public synchronized boolean isInFlight() {
        return inFlight;
    }

    public synchronized boolean hasPending() {
        return pendingNewest != null;
    }

    public synchronized boolean isHelloOk() {
        return helloOk;
    }

    /**
     * Lab hook: emit an in-flight FRAME_PATCH with a wrong base id so the
     * real receiver can NACK_BASE and the sender keyframe-resyncs.
     */
    public synchronized boolean debugInjectWrongBase() {
        if (!helloOk || inFlight || ackedFrame == null) return false;
        byte[] target = pendingNewest != null ? pendingNewest : ackedFrame;
        PatchRect rect = new PatchRect(0, 0, 16, 16, Arrays.copyOf(target, 32));
        FramePatch rogue = new FramePatch(nextFrameId, ackedFrameId + 97, false, 1,
                FrameDiffer.TOTAL_TILES, Collections.singletonList(rect));
        emit(M4B3Codec.encodePatch(rogue, target, nextSeq++));
        recordSend(rogue, target, false);
        return true;
    }

    /**
     * Lab hook: emit a geometrically valid patch with a corrupted CRC so the
     * receiver NACK_CRC path can be proven on the real link.
     */
    public synchronized boolean debugInjectCorruptCrc() {
        if (!helloOk || inFlight || ackedFrame == null) return false;
        byte[] target = Arrays.copyOf(ackedFrame, ackedFrame.length);
        LogicalMonoFrame.setBlack(target, 0, 0, !LogicalMonoFrame.isBlack(target, 0, 0));
        FramePatch patch = FrameDiffer.diff(ackedFrame, ackedFrameId, target, nextFrameId);
        if (!patch.hasChanges() || patch.keyframe) return false;
        byte[] wire = M4B3Codec.encodePatch(patch, target, nextSeq++);
        // CRC is the last 4 bytes of the 14-byte patch header.
        int crcOff = M4B3.ENVELOPE_SIZE + M4B3.PATCH_HEADER_SIZE - 1;
        wire[crcOff] = (byte) (wire[crcOff] ^ 0xFF);
        emit(wire);
        recordSend(patch, target, false);
        return true;
    }

    private void onHello(M4B3Message.Hello hello) {
        if (hello.version != M4B3.VERSION) {
            helloOk = false;
            return;
        }
        if (hello.status != M4B3.HELLO_OK
                || !M4B3.sameLogicalFormat(hello.width, hello.height, hello.pixelFormat, hello.stride)) {
            helloOk = false;
            return;
        }
        boolean first = !helloOk;
        helloOk = true;
        if (first) considerPending();
    }

    private void onAck(M4B3Message.Ack ack) {
        if (!inFlight) return;
        boolean mismatch = ack.frameId != inFlightFrameId
                || !ack.ok()
                || ack.acceptedFrameId != inFlightFrameId;
        if (mismatch) {
            forceKeyframe = true;
            nackRecoveries++;
            if (pendingNewest == null) pendingNewest = inFlightFrame;
            clearInFlight();
            considerPending();
            return;
        }
        ackedFrame = inFlightFrame;
        ackedFrameId = inFlightFrameId;
        ackedCrc = inFlightCrc;
        forceKeyframe = false;
        clearInFlight();
        considerPending();
    }

    private void considerPending() {
        if (!helloOk || inFlight || pendingNewest == null) return;
        byte[] next = pendingNewest;
        pendingNewest = null;
        trySend(next);
    }

    private void trySend(byte[] target) {
        if (forceKeyframe || ackedFrame == null) {
            sendKeyframe(target);
            return;
        }
        long frameId = nextFrameId;
        FramePatch patch = FrameDiffer.diff(ackedFrame, ackedFrameId, target, frameId);
        if (!patch.hasChanges()) {
            unchangedSuppressed++;
            lastChangedTiles = 0;
            lastRectCount = 0;
            lastPatchBytes = 0;
            lastDirtyRatio = 0;
            return;
        }
        if (patch.keyframe) {
            sendKeyframe(target);
            return;
        }
        emit(M4B3Codec.encodePatch(patch, target, nextSeq++));
        recordSend(patch, target, false);
    }

    private void sendKeyframe(byte[] target) {
        long frameId = nextFrameId;
        FramePatch patch = FrameDiffer.diff(null, -1, target, frameId);
        emit(M4B3Codec.encodeKeyframe(frameId, target, nextSeq++));
        recordSend(patch, target, true);
    }

    private void recordSend(FramePatch patch, byte[] target, boolean key) {
        inFlight = true;
        inFlightFrame = target;
        inFlightFrameId = patch.frameId;
        inFlightCrc = M4B3.framebufferCrc32(target);
        nextFrameId++;
        if (key) keyframesSent++;
        else patchesSent++;
        payloadBytesSent += patch.payloadBytes();
        lastChangedTiles = patch.changedTiles;
        lastRectCount = patch.rects.size();
        lastPatchBytes = patch.payloadBytes();
        lastDirtyRatio = patch.dirtyRatio();
    }

    private void clearInFlight() {
        inFlight = false;
        inFlightFrame = null;
        inFlightFrameId = -1;
        inFlightCrc = 0;
    }

    private void beginSession(boolean keepForceKeyframe) {
        byte[] snapshot = pendingNewest != null ? pendingNewest : lastOffered;
        resetSession(keepForceKeyframe);
        pendingNewest = snapshot;
        lastOffered = snapshot;
        emit(M4B3Codec.encodeV1Hello(nextSeq++, M4B3.HELLO_OK));
        hellosSent++;
    }

    private void resetSession(boolean keepForceKeyframe) {
        helloOk = false;
        forceKeyframe = keepForceKeyframe;
        clearInFlight();
        nextFrameId = 0;
        ackedFrameId = -1;
        ackedCrc = 0;
        ackedFrame = null;
        pendingNewest = null;
        lastOffered = null;
        lastChangedTiles = 0;
        lastRectCount = 0;
        lastPatchBytes = 0;
        lastDirtyRatio = 0;
    }

    private void emit(byte[] packet) {
        outbound.enqueue(packet);
    }

    public static final class Stats {
        public final boolean helloOk;
        public final boolean forceKeyframe;
        public final boolean inFlight;
        public final boolean hasPending;
        public final long ackedFrameId;
        public final long inFlightFrameId;
        public final long nextFrameId;
        public final int ackedCrc;
        public final int inFlightCrc;
        public final long keyframesSent;
        public final long patchesSent;
        public final long unchangedSuppressed;
        public final long nackRecoveries;
        public final long latestWinsDrops;
        public final long hellosSent;
        public final long payloadBytesSent;
        public final int lastChangedTiles;
        public final int lastRectCount;
        public final int lastPatchBytes;
        public final double lastDirtyRatio;

        Stats(boolean helloOk, boolean forceKeyframe, boolean inFlight, boolean hasPending,
                long ackedFrameId, long inFlightFrameId, long nextFrameId,
                int ackedCrc, int inFlightCrc,
                long keyframesSent, long patchesSent, long unchangedSuppressed,
                long nackRecoveries, long latestWinsDrops, long hellosSent, long payloadBytesSent,
                int lastChangedTiles, int lastRectCount, int lastPatchBytes, double lastDirtyRatio) {
            this.helloOk = helloOk;
            this.forceKeyframe = forceKeyframe;
            this.inFlight = inFlight;
            this.hasPending = hasPending;
            this.ackedFrameId = ackedFrameId;
            this.inFlightFrameId = inFlightFrameId;
            this.nextFrameId = nextFrameId;
            this.ackedCrc = ackedCrc;
            this.inFlightCrc = inFlightCrc;
            this.keyframesSent = keyframesSent;
            this.patchesSent = patchesSent;
            this.unchangedSuppressed = unchangedSuppressed;
            this.nackRecoveries = nackRecoveries;
            this.latestWinsDrops = latestWinsDrops;
            this.hellosSent = hellosSent;
            this.payloadBytesSent = payloadBytesSent;
            this.lastChangedTiles = lastChangedTiles;
            this.lastRectCount = lastRectCount;
            this.lastPatchBytes = lastPatchBytes;
            this.lastDirtyRatio = lastDirtyRatio;
        }
    }
}
