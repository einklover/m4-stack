package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.patch.FrameDiffer;
import com.murphy.m4screenbridge.browser.patch.FramePatch;
import com.murphy.m4screenbridge.browser.patch.LogicalMonoFrame;
import com.murphy.m4screenbridge.browser.stream.M4B3;
import com.murphy.m4screenbridge.browser.stream.M4B3Codec;
import com.murphy.m4screenbridge.browser.stream.M4B3Exception;
import com.murphy.m4screenbridge.browser.stream.M4B3Message;
import com.murphy.m4screenbridge.browser.stream.M4B3ReferenceReceiver;
import com.murphy.m4screenbridge.browser.stream.M4B3Sender;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/** Pure-Java M4B3 codec, reference receiver, and sender state-machine checks. */
public final class M4B3ProtocolTest {
    static final long LONG_RUN_SEED = 0x4D344233L;
    static final int LONG_RUN_FRAMES = 300;

    public static void main(String[] args) {
        crc32CheckValue();
        helloRoundtripAndEndianness();
        helloVersionRejection();
        keyframeRoundtrip();
        sparsePatchRoundtrip();
        denseKeyframeFallback();
        noChangeSuppression();
        wrongBaseNackForcesKeyframe();
        crcCorruptionRejected();
        truncatedMessagesRejected();
        oversizedAndOverflowRejected();
        invalidRectanglesRejected();
        reconnectHelloAndKeyframe();
        oneFrameInFlight();
        delayedAckLatestWins();
        pingPong();
        randomizedLongRun();
        System.out.println("OK: M4B3 protocol self-checks passed");
    }

    private static void crc32CheckValue() {
        byte[] abc = "123456789".getBytes(StandardCharsets.US_ASCII);
        assertEquals(M4B3.CRC32_CHECK, M4B3.crc32(abc), "IEEE CRC-32 check value");
        assertEquals(M4B3.framebufferCrc32(LogicalMonoFrame.white()),
                M4B3.crc32(LogicalMonoFrame.white()), "framebuffer CRC delegates");
    }

    private static void helloRoundtripAndEndianness() {
        byte[] wire = M4B3Codec.encodeV1Hello(7, M4B3.HELLO_OK);
        assertEquals('M', wire[0] & 0xFF, "magic0");
        assertEquals('4', wire[1] & 0xFF, "magic1");
        assertEquals('B', wire[2] & 0xFF, "magic2");
        assertEquals('3', wire[3] & 0xFF, "magic3");
        assertEquals(M4B3.TYPE_HELLO, wire[4] & 0xFF, "HELLO type");
        assertEquals(M4B3.HELLO_HEADER_SIZE, (wire[6] & 0xFF) | ((wire[7] & 0xFF) << 8),
                "header_len LE");
        assertEquals(0, leU32(wire, 8), "payload_len");
        assertEquals(7, leU32(wire, 12), "seq LE");
        assertEquals(480, (wire[17] & 0xFF) | ((wire[18] & 0xFF) << 8), "width LE");
        assertEquals(800, (wire[19] & 0xFF) | ((wire[20] & 0xFF) << 8), "height LE");
        assertEquals(M4B3.PIXEL_MONO1, wire[21] & 0xFF, "pixel format");
        assertEquals(60, (wire[22] & 0xFF) | ((wire[23] & 0xFF) << 8), "stride LE");

        M4B3Message parsed = M4B3Codec.parse(wire);
        assertEquals(M4B3.TYPE_HELLO, parsed.type, "parsed type");
        assertTrue(parsed.hello.compatibleV1(), "v1 hello compatible");
        assertEquals(7, parsed.seq, "parsed seq");
        byte[] again = M4B3Codec.encodeHello(parsed.hello, parsed.seq);
        assertTrue(Arrays.equals(wire, again), "HELLO encode/decode identity");
    }

    private static void helloVersionRejection() {
        M4B3Message.Hello bad = new M4B3Message.Hello(
                2, M4B3.WIDTH, M4B3.HEIGHT, M4B3.PIXEL_MONO1, M4B3.STRIDE,
                M4B3.V1_CAPABILITIES, M4B3.MAX_PAYLOAD_LEN, M4B3.HELLO_OK);
        byte[] wire = M4B3Codec.encodeHello(bad, 1);
        M4B3Message parsed = M4B3Codec.parse(wire);
        assertEquals(2, parsed.hello.version, "version 2 parses as HELLO");
        assertTrue(!parsed.hello.compatibleV1(), "version 2 is not v1");

        M4B3ReferenceReceiver recv = new M4B3ReferenceReceiver();
        List<byte[]> replies = recv.handle(wire);
        assertEquals(1, replies.size(), "version reject replies");
        M4B3Message reply = M4B3Codec.parse(replies.get(0));
        assertEquals(M4B3.TYPE_HELLO, reply.type, "reject is HELLO");
        assertEquals(M4B3.HELLO_UNSUPPORTED_VERSION, reply.hello.status, "unsupported version");
        assertTrue(!recv.isHelloAccepted(), "receiver does not accept v2");
        assertTrue(recv.copyFramebuffer() == null, "no framebuffer after rejected hello");

        Link link = new Link();
        link.sender.connect();
        link.deliverSenderToReceiver();
        // Replace the OK reply with a version-reject HELLO.
        link.sender.receive(replies.get(0));
        assertTrue(!link.sender.isHelloOk(), "sender rejects incompatible HELLO");
        link.sender.offerFrame(LogicalMonoFrame.white());
        assertEquals(0, countTypes(link.sent, M4B3.TYPE_FRAME_KEY),
                "no keyframe before compatible hello");
    }

    private static void keyframeRoundtrip() {
        Link link = new Link();
        link.connectAndHandshake();
        byte[] target = LogicalMonoFrame.white();
        paintRect(target, 32, 80, 160, 48, true);
        link.sender.offerFrame(target);
        assertEquals(1, countTypes(link.sent, M4B3.TYPE_FRAME_KEY), "first frame is key");
        M4B3Message key = lastOfType(link.sent, M4B3.TYPE_FRAME_KEY);
        assertEquals(0, key.keyframe.frameId, "first frame_id");
        assertEquals(LogicalMonoFrame.SIZE, key.keyframe.payload.length, "key payload 48000");
        assertEquals(M4B3.framebufferCrc32(target), key.keyframe.crc32, "key CRC");
        link.pump();
        assertArray(target, link.recv.copyFramebuffer(), "keyframe reconstruction");
        assertEquals(M4B3.framebufferCrc32(target), link.recv.acceptedCrc(), "key CRC committed");
        assertEquals(0, link.recv.acceptedFrameId(), "accepted frame 0");
        assertEquals(0, link.recv.nacksSent(), "no nack on valid key");
    }

    private static void sparsePatchRoundtrip() {
        Link link = primedWhite();
        byte[] target = LogicalMonoFrame.white();
        for (int line = 0; line < 5; line++) {
            int y = 96 + line * 48;
            paintRect(target, 32, y, 256, 10, true);
            paintRect(target, 320, y, 96, 10, true);
        }
        int before = countTypes(link.sent, M4B3.TYPE_FRAME_PATCH);
        link.sender.offerFrame(target);
        assertEquals(before + 1, countTypes(link.sent, M4B3.TYPE_FRAME_PATCH), "sparse is patch");
        M4B3Message patch = lastOfType(link.sent, M4B3.TYPE_FRAME_PATCH);
        assertEquals(0, patch.patch.baseFrameId, "patch base is ACKed 0");
        assertTrue(patch.patch.rects.size() > 0, "sparse has rects");
        assertTrue(patch.patch.rects.size() < 40, "sparse rects stay bounded");
        link.pump();
        assertArray(target, link.recv.copyFramebuffer(), "sparse reconstruction");
        assertEquals(M4B3.framebufferCrc32(target), link.recv.acceptedCrc(), "sparse CRC");
        assertEquals(1, link.recv.patchesApplied(), "one patch applied");
    }

    private static void denseKeyframeFallback() {
        Link link = primedWhite();
        byte[] target = new byte[LogicalMonoFrame.SIZE];
        FramePatch local = FrameDiffer.diff(LogicalMonoFrame.white(), 0, target, 1);
        assertTrue(local.keyframe, "local differ says keyframe");
        link.sender.offerFrame(target);
        assertEquals(0, countTypesSince(link.sent, link.mark, M4B3.TYPE_FRAME_PATCH),
                "dense change is not a patch");
        assertEquals(1, countTypesSince(link.sent, link.mark, M4B3.TYPE_FRAME_KEY),
                "dense change is a keyframe");
        link.pump();
        assertArray(target, link.recv.copyFramebuffer(), "dense key reconstruction");
        assertEquals(2, link.recv.keysApplied(), "second key applied");
    }

    private static void noChangeSuppression() {
        Link link = primedWhite();
        long nextBefore = link.sender.stats().nextFrameId;
        int framesBefore = countFrames(link.sent);
        link.sender.offerFrame(LogicalMonoFrame.white());
        assertEquals(framesBefore, countFrames(link.sent), "no-change does not transmit");
        assertEquals(nextBefore, link.sender.stats().nextFrameId, "no-change does not advance id");
        assertEquals(1, link.sender.stats().unchangedSuppressed, "no-change counted");
        assertTrue(!link.sender.isInFlight(), "no-change leaves sender idle");
        link.pump();
        assertEquals(0, link.recv.acceptedFrameId(), "accepted id stays 0");
    }

    private static void wrongBaseNackForcesKeyframe() {
        Link link = primedWhite();
        byte[] next = LogicalMonoFrame.white();
        paintRect(next, 16, 16, 32, 32, true);
        link.sender.offerFrame(next);
        M4B3Message patch = lastOfType(link.sent, M4B3.TYPE_FRAME_PATCH);
        assertEquals(0, patch.patch.baseFrameId, "patch based on 0");
        link.dropInbox();

        byte[] snapshot = link.recv.copyFramebuffer();
        int crcBefore = link.recv.acceptedCrc();
        // Deliver a patch that claims the wrong base without touching sender's wire.
        FramePatch rogue = FrameDiffer.diff(next, 99, flipPixel(next, 64, 64), 100);
        byte[] rogueWire = M4B3Codec.encodePatch(rogue, flipPixel(next, 64, 64), 99);
        List<byte[]> replies = link.recv.handle(rogueWire);
        M4B3Message nack = M4B3Codec.parse(replies.get(0));
        assertEquals(M4B3.TYPE_FRAME_ACK, nack.type, "wrong base produces ACK");
        assertEquals(M4B3.NACK_BASE, nack.ack.result, "NACK_BASE");
        assertEquals(0, nack.ack.acceptedFrameId, "accepted stays 0");
        assertArray(snapshot, link.recv.copyFramebuffer(), "wrong base does not mutate");
        assertEquals(crcBefore, link.recv.acceptedCrc(), "CRC unchanged after NACK_BASE");

        int keysBefore = countTypes(link.sent, M4B3.TYPE_FRAME_KEY);
        link.sender.receive(replies.get(0));
        assertTrue(link.sender.stats().forceKeyframe
                || countTypes(link.sent, M4B3.TYPE_FRAME_KEY) > keysBefore,
                "NACK invalidates confidence");
        assertEquals(keysBefore + 1, countTypes(link.sent, M4B3.TYPE_FRAME_KEY),
                "NACK forces keyframe");
        link.pump();
        assertArray(next, link.recv.copyFramebuffer(), "keyframe resynchronizes");
        assertEquals(M4B3.framebufferCrc32(next), link.recv.acceptedCrc(), "resync CRC");
        assertTrue(link.recv.keysApplied() >= 2, "recovery key applied");
    }

    private static void crcCorruptionRejected() {
        Link link = primedWhite();
        byte[] next = LogicalMonoFrame.white();
        paintRect(next, 80, 200, 64, 16, true);
        link.sender.offerFrame(next);
        byte[] good = lastPacketOfType(link.sent, M4B3.TYPE_FRAME_PATCH);
        link.dropInbox();
        byte[] corrupt = Arrays.copyOf(good, good.length);
        corrupt[corrupt.length - 1] = (byte) (corrupt[corrupt.length - 1] ^ 0xFF);

        byte[] before = link.recv.copyFramebuffer();
        List<byte[]> replies = link.recv.handle(corrupt);
        M4B3Message nack = M4B3Codec.parse(replies.get(0));
        assertEquals(M4B3.NACK_CRC, nack.ack.result, "CRC corruption is NACK_CRC");
        assertArray(before, link.recv.copyFramebuffer(), "CRC fail does not commit");
        assertEquals(0, link.recv.acceptedFrameId(), "accepted stays previous");

        int keysBefore = countTypes(link.sent, M4B3.TYPE_FRAME_KEY);
        link.sender.receive(replies.get(0));
        assertEquals(keysBefore + 1, countTypes(link.sent, M4B3.TYPE_FRAME_KEY),
                "CRC NACK forces keyframe");
        link.pump();
        assertArray(next, link.recv.copyFramebuffer(), "CRC recovery key matches newest");
    }

    private static void truncatedMessagesRejected() {
        byte[] hello = M4B3Codec.encodeV1Hello(1, M4B3.HELLO_OK);
        expectKind(M4B3Exception.Kind.TRUNCATED, Arrays.copyOf(hello, 10), "short envelope");
        expectKind(M4B3Exception.Kind.TRUNCATED, Arrays.copyOf(hello, 15), "15-byte envelope");

        byte[] key = M4B3Codec.encodeKeyframe(0, LogicalMonoFrame.white(), 2);
        expectKind(M4B3Exception.Kind.TRUNCATED, Arrays.copyOf(key, M4B3.ENVELOPE_SIZE),
                "key envelope only");
        expectKind(M4B3Exception.Kind.TRUNCATED,
                Arrays.copyOf(key, M4B3.ENVELOPE_SIZE + M4B3.KEY_HEADER_SIZE),
                "key header without payload");
        expectKind(M4B3Exception.Kind.TRUNCATED, Arrays.copyOf(key, key.length - 1),
                "key body truncated one byte");

        byte[] base = LogicalMonoFrame.white();
        byte[] target = Arrays.copyOf(base, base.length);
        LogicalMonoFrame.setBlack(target, 8, 8, true);
        FramePatch patch = FrameDiffer.diff(base, 0, target, 1);
        byte[] patchWire = M4B3Codec.encodePatch(patch, target, 3);
        expectKind(M4B3Exception.Kind.TRUNCATED, Arrays.copyOf(patchWire, patchWire.length - 1),
                "patch payload truncated");

        M4B3ReferenceReceiver recv = new M4B3ReferenceReceiver();
        recv.handle(M4B3Codec.encodeV1Hello(0, M4B3.HELLO_OK));
        recv.handle(M4B3Codec.encodeKeyframe(0, base, 1));
        byte[] committed = recv.copyFramebuffer();
        recv.handle(Arrays.copyOf(patchWire, patchWire.length - 5));
        assertArray(committed, recv.copyFramebuffer(), "truncated patch does not apply");
        assertEquals(0, recv.acceptedFrameId(), "truncated leaves accepted id");
    }

    private static void oversizedAndOverflowRejected() {
        byte[] hugePayload = M4B3Codec.wrap(M4B3.TYPE_FRAME_KEY, 0, new byte[M4B3.KEY_HEADER_SIZE],
                new byte[0], 1, null, (long) (M4B3.MAX_PAYLOAD_LEN + 1));
        expectKind(M4B3Exception.Kind.OVERSIZED, hugePayload, "payload_len MAX+1");

        byte[] hugeHeader = M4B3Codec.wrap(M4B3.TYPE_HELLO, 0, new byte[0], new byte[0], 1,
                0xFFFF, 0L);
        expectKind(M4B3Exception.Kind.OVERSIZED, hugeHeader, "header_len 0xFFFF");

        byte[] u32Max = M4B3Codec.wrap(M4B3.TYPE_FRAME_KEY, 0, new byte[0], new byte[0], 1,
                0xFFFF, 0xFFFFFFFFL);
        expectKind(M4B3Exception.Kind.OVERSIZED, u32Max, "u32 max lengths rejected before alloc");

        // Lying rect payload that would overflow a 32-bit width*height product.
        byte[] overflowRect = craftPatch(1, 0, 1,
                new int[] {0, 0, 0xFFF8, 0xFFFF, 0xFFFFFFFF});
        expectKind(M4B3Exception.Kind.OVERFLOW, overflowRect, "rect payload overflow");

        byte[] tooManyRects = craftPatchHeaderOnly(1, 0, 2000, 0);
        expectKind(M4B3Exception.Kind.OVERSIZED, tooManyRects, "rect_count 2000");
    }

    private static void invalidRectanglesRejected() {
        expectKind(M4B3Exception.Kind.INVALID,
                craftPatch(1, 0, 1, new int[] {1, 0, 8, 16, 16}),
                "x not byte-aligned");
        expectKind(M4B3Exception.Kind.INVALID,
                craftPatch(1, 0, 1, new int[] {0, 0, 0, 16, 0}),
                "zero width");
        expectKind(M4B3Exception.Kind.INVALID,
                craftPatch(1, 0, 1, new int[] {400, 0, 96, 16, 192}),
                "rect extends past 480");
        expectKind(M4B3Exception.Kind.INVALID,
                craftPatch(1, 0, 1, new int[] {0, 0, 16, 16, 4}),
                "rect payload_len != geometry");

        M4B3ReferenceReceiver recv = new M4B3ReferenceReceiver();
        byte[] white = LogicalMonoFrame.white();
        recv.handle(M4B3Codec.encodeV1Hello(0, M4B3.HELLO_OK));
        recv.handle(M4B3Codec.encodeKeyframe(0, white, 1));
        byte[] before = recv.copyFramebuffer();
        recv.handle(craftPatch(1, 0, 1, new int[] {1, 0, 8, 16, 16}));
        assertArray(before, recv.copyFramebuffer(), "invalid rect does not apply");
        assertEquals(0, recv.acceptedFrameId(), "invalid rect leaves accepted id");
    }

    private static void reconnectHelloAndKeyframe() {
        Link link = primedWhite();
        byte[] next = LogicalMonoFrame.white();
        paintRect(next, 0, 400, 480, 16, true);
        link.sender.offerFrame(next);
        link.pump();
        assertArray(next, link.recv.copyFramebuffer(), "pre-reconnect state");

        int helloBefore = countTypes(link.sent, M4B3.TYPE_HELLO);
        int keyBefore = countTypes(link.sent, M4B3.TYPE_FRAME_KEY);
        link.mark = link.sent.size();
        link.sender.reconnect();
        List<M4B3Message> after = since(link.sent, link.mark);
        assertEquals(M4B3.TYPE_HELLO, after.get(0).type, "reconnect starts with HELLO");
        assertEquals(helloBefore + 1, countTypes(link.sent, M4B3.TYPE_HELLO), "one new HELLO");
        link.pump();
        assertEquals(keyBefore + 1, countTypes(link.sent, M4B3.TYPE_FRAME_KEY),
                "reconnect sends keyframe");
        assertEquals(M4B3.TYPE_FRAME_KEY, lastOfType(link.sent, M4B3.TYPE_FRAME_KEY).type,
                "post-hello frame is key");
        assertArray(next, link.recv.copyFramebuffer(), "reconnect key restores newest");
        assertEquals(0, lastOfType(link.sent, M4B3.TYPE_FRAME_KEY).keyframe.frameId,
                "reconnect frame ids restart");
    }

    private static void oneFrameInFlight() {
        Link link = new Link();
        link.connectAndHandshake();
        byte[] f0 = LogicalMonoFrame.white();
        paintRect(f0, 0, 0, 16, 16, true);
        link.sender.offerFrame(f0);
        assertEquals(1, countFrames(link.sent), "first capture sends key");
        assertTrue(link.sender.isInFlight(), "one frame in flight");

        byte[] f1 = LogicalMonoFrame.white();
        paintRect(f1, 32, 32, 16, 16, true);
        link.sender.offerFrame(f1);
        assertEquals(1, countFrames(link.sent), "second capture does not send");
        assertTrue(link.sender.hasPending(), "newest retained");

        byte[] f2 = LogicalMonoFrame.white();
        paintRect(f2, 64, 64, 16, 16, true);
        link.sender.offerFrame(f2);
        assertEquals(1, countFrames(link.sent), "third capture still one in flight");
        assertTrue(link.sender.stats().latestWinsDrops >= 1, "intermediate capture dropped");
    }

    private static void delayedAckLatestWins() {
        Link link = new Link();
        link.connectAndHandshake();
        byte[] first = LogicalMonoFrame.white();
        LogicalMonoFrame.setBlack(first, 0, 0, true);
        link.sender.offerFrame(first);
        assertEquals(1, countFrames(link.sent), "only first is on the wire");

        byte[] newest = first;
        for (int i = 1; i <= 24; i++) {
            byte[] frame = LogicalMonoFrame.white();
            LogicalMonoFrame.setBlack(frame, (i * 8) % LogicalMonoFrame.WIDTH, i, true);
            newest = frame;
            link.sender.offerFrame(frame);
        }
        assertEquals(1, countFrames(link.sent), "delayed ACK sends no extra frames");
        assertTrue(link.sender.stats().latestWinsDrops >= 23, "24 captures keep only newest");

        link.pump();
        assertEquals(2, countFrames(link.sent), "ACK releases only the newest frame");
        assertArray(newest, link.recv.copyFramebuffer(), "receiver has newest only");
        assertEquals(M4B3.framebufferCrc32(newest), link.recv.acceptedCrc(), "newest CRC");
        assertEquals(M4B3.framebufferCrc32(newest),
                M4B3.framebufferCrc32(link.sender.copyAckedFramebuffer()),
                "sender/receiver CRC after latest-wins");
    }

    private static void pingPong() {
        byte[] ping = M4B3Codec.encodePing(0xAABBCCDDL, 9);
        M4B3Message parsed = M4B3Codec.parse(ping);
        assertEquals(M4B3.TYPE_PING, parsed.type, "PING type");
        assertEquals(0xAABBCCDDL, parsed.nonce, "PING nonce");
        byte[] pong = M4B3Codec.encodePong(parsed.nonce, 10);
        assertEquals(0xAABBCCDDL, M4B3Codec.parse(pong).nonce, "PONG nonce");

        M4B3ReferenceReceiver recv = new M4B3ReferenceReceiver();
        List<byte[]> replies = recv.handle(ping);
        assertEquals(M4B3.TYPE_PONG, M4B3Codec.parse(replies.get(0)).type, "receiver PONG");
        assertEquals(0xAABBCCDDL, M4B3Codec.parse(replies.get(0)).nonce, "receiver PONG nonce");
    }

    private static void randomizedLongRun() {
        Random rnd = new Random(LONG_RUN_SEED);
        Link link = new Link();
        link.connectAndHandshake();
        byte[] current = LogicalMonoFrame.white();
        link.sender.offerFrame(current);
        link.pump();

        int offers = 1;
        for (int frame = 1; frame < LONG_RUN_FRAMES; frame++) {
            byte[] target = Arrays.copyOf(current, current.length);
            int mode = rnd.nextInt(10);
            if (mode == 0) {
                // no-change
            } else if (mode == 1) {
                Arrays.fill(target, (byte) 0x00);
            } else {
                int edits = 1 + rnd.nextInt(40);
                for (int i = 0; i < edits; i++) {
                    int x = rnd.nextInt(LogicalMonoFrame.WIDTH);
                    int y = rnd.nextInt(LogicalMonoFrame.HEIGHT);
                    LogicalMonoFrame.setBlack(target, x, y,
                            !LogicalMonoFrame.isBlack(target, x, y));
                }
            }
            link.sender.offerFrame(target);
            offers++;
            current = target;
            boolean delay = rnd.nextInt(6) == 0 && link.sender.isInFlight();
            if (!delay) link.pump();
        }
        link.pump();

        byte[] senderFb = link.sender.copyAckedFramebuffer();
        byte[] recvFb = link.recv.copyFramebuffer();
        int senderCrc = M4B3.framebufferCrc32(senderFb);
        int recvCrc = link.recv.acceptedCrc();
        assertArray(current, senderFb, "sender ACKed equals last offered");
        assertArray(senderFb, recvFb, "sender/receiver byte-identical");
        assertEquals(senderCrc, recvCrc, "sender/receiver CRC32");
        System.out.println("M4B3 long-run seed=0x" + Long.toHexString(LONG_RUN_SEED)
                + " frames=" + LONG_RUN_FRAMES
                + " offers=" + offers
                + " key=" + link.sender.stats().keyframesSent
                + " patch=" + link.sender.stats().patchesSent
                + " same=" + link.sender.stats().unchangedSuppressed
                + " latestWins=" + link.sender.stats().latestWinsDrops
                + " nack=" + link.sender.stats().nackRecoveries
                + " senderCrc=" + M4B3.crcHex(senderCrc)
                + " receiverCrc=" + M4B3.crcHex(recvCrc)
                + " identical=true");
    }

    private static Link primedWhite() {
        Link link = new Link();
        link.connectAndHandshake();
        link.sender.offerFrame(LogicalMonoFrame.white());
        link.pump();
        link.mark = link.sent.size();
        return link;
    }

    private static void expectKind(M4B3Exception.Kind kind, byte[] data, String message) {
        try {
            M4B3Codec.parse(data);
            throw new AssertionError(message + ": expected " + kind);
        } catch (M4B3Exception e) {
            if (e.kind != kind) {
                throw new AssertionError(message + ": kind " + e.kind + " expected " + kind
                        + " (" + e.getMessage() + ")");
            }
        }
    }

    private static byte[] craftPatch(long frameId, long baseId, int rectCount, int[] rect) {
        byte[] rectBytes = new byte[M4B3.RECT_META_SIZE];
        ByteBuffer rb = ByteBuffer.wrap(rectBytes).order(ByteOrder.LITTLE_ENDIAN);
        rb.putShort((short) rect[0]);
        rb.putShort((short) rect[1]);
        rb.putShort((short) rect[2]);
        rb.putShort((short) rect[3]);
        rb.putInt(rect[4]);
        byte[] header = new byte[M4B3.PATCH_HEADER_SIZE];
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        h.putInt((int) frameId);
        h.putInt((int) baseId);
        h.putShort((short) rectCount);
        h.putInt(0);
        return M4B3Codec.wrap(M4B3.TYPE_FRAME_PATCH, 0, header, rectBytes, 1);
    }

    private static byte[] craftPatchHeaderOnly(long frameId, long baseId, int rectCount, int crc) {
        byte[] header = new byte[M4B3.PATCH_HEADER_SIZE];
        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        h.putInt((int) frameId);
        h.putInt((int) baseId);
        h.putShort((short) rectCount);
        h.putInt(crc);
        return M4B3Codec.wrap(M4B3.TYPE_FRAME_PATCH, 0, header, new byte[0], 1);
    }

    private static byte[] flipPixel(byte[] src, int x, int y) {
        byte[] out = Arrays.copyOf(src, src.length);
        LogicalMonoFrame.setBlack(out, x, y, !LogicalMonoFrame.isBlack(src, x, y));
        return out;
    }

    private static void paintRect(byte[] frame, int x, int y, int width, int height, boolean black) {
        for (int yy = y; yy < y + height; yy++) {
            for (int xx = x; xx < x + width; xx++) {
                LogicalMonoFrame.setBlack(frame, xx, yy, black);
            }
        }
    }

    private static long leU32(byte[] d, int off) {
        return (d[off] & 0xFFL)
                | ((d[off + 1] & 0xFFL) << 8)
                | ((d[off + 2] & 0xFFL) << 16)
                | ((d[off + 3] & 0xFFL) << 24);
    }

    private static int countTypesSince(List<byte[]> packets, int mark, int type) {
        int n = 0;
        for (int i = mark; i < packets.size(); i++) {
            if (M4B3Codec.parse(packets.get(i)).type == type) n++;
        }
        return n;
    }

    private static int countTypes(List<byte[]> packets, int type) {
        int n = 0;
        for (byte[] p : packets) {
            if (M4B3Codec.parse(p).type == type) n++;
        }
        return n;
    }

    private static int countFrames(List<byte[]> packets) {
        return countTypes(packets, M4B3.TYPE_FRAME_KEY) + countTypes(packets, M4B3.TYPE_FRAME_PATCH);
    }

    private static M4B3Message lastOfType(List<byte[]> packets, int type) {
        M4B3Message last = null;
        for (byte[] p : packets) {
            M4B3Message m = M4B3Codec.parse(p);
            if (m.type == type) last = m;
        }
        if (last == null) throw new AssertionError("missing " + M4B3.typeName(type));
        return last;
    }

    private static byte[] lastPacketOfType(List<byte[]> packets, int type) {
        byte[] last = null;
        for (byte[] p : packets) {
            if (M4B3Codec.parse(p).type == type) last = p;
        }
        if (last == null) throw new AssertionError("missing packet " + M4B3.typeName(type));
        return last;
    }

    private static List<M4B3Message> since(List<byte[]> packets, int mark) {
        List<M4B3Message> out = new ArrayList<>();
        for (int i = mark; i < packets.size(); i++) out.add(M4B3Codec.parse(packets.get(i)));
        return out;
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

    private static void assertEquals(long expected, long actual, String message) {
        if (expected != actual) {
            throw new AssertionError(message + ": expected=" + expected + " actual=" + actual);
        }
    }

    private static final class Link {
        final List<byte[]> sent = new ArrayList<>();
        final List<byte[]> inbox = new ArrayList<>();
        final M4B3Sender sender;
        final M4B3ReferenceReceiver recv = new M4B3ReferenceReceiver();
        int mark;

        Link() {
            sender = new M4B3Sender(packet -> {
                sent.add(packet);
                inbox.add(packet);
            });
        }

        void connectAndHandshake() {
            sender.connect();
            pump();
            assertTrue(sender.isHelloOk(), "sender hello");
            assertTrue(recv.isHelloAccepted(), "receiver hello");
        }

        void dropInbox() {
            inbox.clear();
        }

        void deliverSenderToReceiver() {
            List<byte[]> batch = new ArrayList<>(inbox);
            inbox.clear();
            for (byte[] packet : batch) {
                for (byte[] reply : recv.handle(packet)) {
                    sender.receive(reply);
                }
            }
        }

        void pump() {
            int guard = 0;
            while (!inbox.isEmpty()) {
                if (++guard > 64) throw new AssertionError("pump did not settle");
                deliverSenderToReceiver();
            }
        }
    }
}
