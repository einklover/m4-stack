package com.murphy.m4screenbridge;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Random;

/** Off-device self-check. Run via build.sh --test (no Android dependencies). */
public final class TestMain {
    public static void main(String[] args) {
        rleRoundtrip();
        headerLayout();
        packing();
        autoLayout();
        autoLayoutOverflow();
        textPageGuard();
        gapCompression();
        grayGapCompression();
        thresholdSanity();
        xhsFeedCards();
        System.out.println("OK: all self-checks passed");
    }

    private static void xhsFeedCards() {
        XhsFeedParser.Item note = XhsFeedParser.parse(
                "笔记  2026上半年纯电动汽车销量TOP40 来自中汽数研 645赞");
        assertTrue(note != null, "xhs text note accepted");
        assertEquals("2026上半年纯电动汽车销量TOP40", note.title, "xhs title");
        assertEquals("中汽数研", note.author, "xhs author");
        assertEquals("645", note.likes, "xhs likes");
        assertEquals(XhsFeedParser.stableToken(note.title, note.author),
                XhsFeedParser.stableToken(note.title, note.author), "xhs token stable");
        assertTrue(!XhsFeedParser.stableToken(note.title, note.author).equals(
                XhsFeedParser.stableToken(note.title + "2", note.author)), "xhs token distinguishes title");
        assertTrue(XhsFeedParser.parse("视频  麒麟四足机器人首发 来自抖科技 1763赞") == null,
                "xhs video rejected");
        assertTrue(XhsFeedParser.parse("直播  今晚八点开播 来自主理人 8赞") == null,
                "xhs live rejected");
    }

    private static void rleRoundtrip() {
        Random rnd = new Random(42);
        byte[] data = makePageBytes(rnd);
        byte[] enc = Rle.encode(data);
        assertTrue(Rle.decode(enc, data.length).length == data.length, "rle decode size");
        assertTrue(Arrays.equals(data, Rle.decode(enc, data.length)), "rle roundtrip");
        assertTrue(enc.length < data.length, "rle compresses text page: " + enc.length + " vs " + data.length);
        assertTrue(Rle.codecFor(data, enc) == Rle.CODEC_RLE1, "codec rle selected");

        for (int t = 0; t < 200; t++) {
            byte[] d = new byte[1 + rnd.nextInt(48000)];
            for (int i = 0; i < d.length; i++) d[i] = (byte) rnd.nextInt(256);
            byte[] e = Rle.encode(d);
            assertTrue(Arrays.equals(d, Rle.decode(e, d.length)), "rle random roundtrip");
        }

        byte[] allWhite = new byte[48000];
        Arrays.fill(allWhite, (byte) 0xFF);
        assertTrue(Arrays.equals(allWhite, Rle.decode(Rle.encode(allWhite), allWhite.length)),
                "rle all-white roundtrip");
        byte[] single = {5, 5, 6, 7, 7, 7, 7};
        assertTrue(Arrays.equals(single, Rle.decode(Rle.encode(single), single.length)),
                "rle short roundtrip");
    }

    private static void headerLayout() {
        byte[] h = Header.build(Rle.CODEC_RLE1, 800, 480, 100, 7, 48000, 0x12345678L);
        assertEquals(Header.SIZE, h.length, "header size");
        assertEquals("M4R1", new String(h, 0, 4, StandardCharsets.US_ASCII), "magic");
        assertEquals(1, h[4] & 0xFF, "version");
        assertEquals(1, h[5] & 0xFF, "codec");
        assertEquals(800, (h[6] & 0xFF) | ((h[7] & 0xFF) << 8), "width");
        assertEquals(480, (h[8] & 0xFF) | ((h[9] & 0xFF) << 8), "height");
        assertEquals(100, (h[10] & 0xFF) | ((h[11] & 0xFF) << 8), "stride");
        assertEquals(7, leInt(h, 12), "page");
        assertEquals(48000, leInt(h, 16), "rawSize");
        assertEquals((int) 0x12345678L, leInt(h, 20), "crc32");

        int[] f = Header.parse(h);
        assertTrue(f != null, "header parses");
        assertEquals(1, f[0], "parsed version");
        assertEquals(1, f[1], "parsed codec");
        assertEquals(800, f[2], "parsed width");
        assertEquals(480, f[3], "parsed height");
        assertEquals(100, f[4], "parsed stride");
        assertEquals(7, f[5], "parsed page");
        assertEquals(48000, f[6], "parsed rawSize");
        assertEquals((int) 0x12345678L, f[7], "parsed crc32");
    }

    private static void packing() {
        boolean[][] page = new boolean[800][480];
        byte[] fb = Framebuffer.pack(page);
        for (byte b : fb) assertEquals((byte) 0xFF, b, "all white init 1");

        page[0][0] = true;
        fb = Framebuffer.pack(page);
        assertEquals((byte) 0x7F, fb[479 * 100 + 0], "logical (0,0) -> phy x=0,y=479 bit7");
        assertTrue(Framebuffer.logicalPixel(fb, 0, 0), "logicalPixel (0,0) black");
        assertTrue(!Framebuffer.logicalPixel(fb, 1, 0), "logicalPixel (1,0) white");

        page = new boolean[800][480];
        page[799][479] = true;
        fb = Framebuffer.pack(page);
        assertEquals((byte) 0xFE, fb[0 * 100 + 99], "logical (479,799) -> phy x=799,y=0 bit0");
        assertTrue(Framebuffer.logicalPixel(fb, 479, 799), "logicalPixel (479,799) black");

        Random r = new Random(7);
        page = new boolean[800][480];
        for (int i = 0; i < 5000; i++) page[r.nextInt(800)][r.nextInt(480)] = true;
        fb = Framebuffer.pack(page);
        for (int y = 0; y < 800; y++) {
            for (int x = 0; x < 480; x++) {
                assertEquals(page[y][x], Framebuffer.logicalPixel(fb, x, y), "pack roundtrip");
            }
        }
        assertEquals(Framebuffer.RAW_SIZE, fb.length, "raw size");
    }

    private static void autoLayout() {
        int sourceH = 1000;
        int[] gray = new int[480 * sourceH];
        Arrays.fill(gray, 255);
        for (int y = 65; y < 70; y++) {
            for (int x = 200; x < 260; x++) gray[y * 480 + x] = 0;
        }
        for (int y = 80; y < 100; y++) {
            for (int x = 20; x < 460; x++) gray[y * 480 + x] = 0;
        }
        for (int y = 300; y < 320; y++) {
            for (int x = 20; x < 460; x++) gray[y * 480 + x] = 0;
        }
        int[] laidOut = Preprocess.autoLayout(gray, sourceH, 128, 12);
        assertEquals(480 * 800, laidOut.length, "auto layout size");
        assertEquals(0, laidOut[372 * 480 + 20], "first row is complete");
        assertEquals(0, laidOut[408 * 480 + 20], "second row is complete");
        assertEquals(255, laidOut[0], "safe top margin");
        int bands = 0;
        boolean previousInk = false;
        for (int y = 0; y < 800; y++) {
            boolean ink = false;
            for (int x = 0; x < 480; x++) ink |= laidOut[y * 480 + x] < 128;
            if (ink && !previousInk) bands++;
            previousInk = ink;
        }
        assertEquals(2, bands, "small leading UI label removed");
    }

    private static void autoLayoutOverflow() {
        int sourceH = 1200;
        int[] gray = new int[480 * sourceH];
        Arrays.fill(gray, 255);
        for (int line = 0; line < 20; line++) {
            int y0 = 80 + line * 53;
            for (int y = y0; y < y0 + 43; y++) {
                for (int x = 100; x < 380; x++) gray[y * 480 + x] = 0;
            }
        }
        int[] laidOut = Preprocess.autoLayout(gray, sourceH, 128, 12);
        int bands = 0;
        int firstInk = -1;
        int lastInk = -1;
        boolean previousInk = false;
        for (int y = 0; y < 800; y++) {
            boolean ink = false;
            for (int x = 0; x < 480; x++) ink |= laidOut[y * 480 + x] < 128;
            if (ink) {
                if (firstInk < 0) firstInk = y;
                lastInk = y;
            }
            if (ink && !previousInk) bands++;
            previousInk = ink;
        }
        assertEquals(20, bands, "all overflowing lines retained");
        assertTrue(firstInk >= 24, "top safety margin");
        assertTrue(lastInk < 776, "bottom safety margin");
    }

    private static void textPageGuard() {
        int sourceH = 1000;
        int[] text = new int[480 * sourceH];
        Arrays.fill(text, 255);
        for (int line = 0; line < 10; line++) {
            int y0 = 100 + line * 70;
            for (int y = y0; y < y0 + 28; y++) {
                for (int x = 30; x < 450; x += 24) {
                    for (int xx = x; xx < x + 12; xx++) text[y * 480 + xx] = 0;
                }
            }
        }
        assertTrue(Preprocess.looksLikeTextPage(text, sourceH, 128), "text page accepted");

        int[] sparseText = new int[480 * sourceH];
        Arrays.fill(sparseText, 255);
        for (int line = 0; line < 2; line++) {
            int y0 = 160 + line * 70;
            int right = line == 0 ? 450 : 260;
            for (int y = y0; y < y0 + 28; y++) {
                for (int x = 30; x < right; x += 24) {
                    for (int xx = x; xx < Math.min(x + 12, right); xx++) {
                        sparseText[y * 480 + xx] = 0;
                    }
                }
            }
        }
        assertTrue(Preprocess.looksLikeTextPage(sparseText, sourceH, 128),
                "sparse final text page accepted");

        int[] popup = new int[480 * sourceH];
        Arrays.fill(popup, 255);
        for (int line = 0; line < 3; line++) {
            int y0 = 350 + line * 55;
            for (int y = y0; y < y0 + 18; y++) {
                for (int x = 190; x < 290; x++) popup[y * 480 + x] = 0;
            }
        }
        assertTrue(!Preprocess.looksLikeTextPage(popup, sourceH, 128),
                "small centered popup rejected");

        int[] ad = new int[480 * sourceH];
        Arrays.fill(ad, 255);
        for (int y = 100; y < 900; y++) {
            for (int x = 20; x < 460; x++) ad[y * 480 + x] = (x + y) % 3 == 0 ? 0 : 100;
        }
        assertTrue(!Preprocess.looksLikeTextPage(ad, sourceH, 128), "dense ad rejected");
    }

    private static void gapCompression() {
        boolean[][] mono = new boolean[800][480];
        for (int y = 0; y < 20; y++) mono[y][10] = true;
        for (int y = 100; y < 120; y++) mono[y][10] = true;
        boolean[][] out = Preprocess.compressGaps(mono, 12);
        assertEquals(800, out.length, "always 800 rows");
        for (int y = 0; y < 20; y++) assertTrue(out[y][10], "first band preserved");
        for (int y = 21; y <= 31; y++) assertTrue(!out[y][10], "gap capped at 12");
        assertTrue(out[32][10], "second band after capped gap");
    }

    private static void thresholdSanity() {
        int[] gray = new int[480 * 800];
        Arrays.fill(gray, 100);
        boolean[][] m = Preprocess.threshold(gray, 128);
        for (int y = 0; y < 800; y++) for (int x = 0; x < 480; x++) assertTrue(m[y][x], "low gray black");
        Arrays.fill(gray, 200);
        m = Preprocess.threshold(gray, 128);
        for (int y = 0; y < 800; y++) for (int x = 0; x < 480; x++) assertTrue(!m[y][x], "high gray white");
    }

    private static void grayGapCompression() {
        int[] gray = new int[480 * 800];
        Arrays.fill(gray, 255);
        for (int y = 10; y < 20; y++) {
            for (int x = 100; x < 120; x++) gray[y * 480 + x] = 0;
        }
        for (int y = 100; y < 110; y++) {
            for (int x = 100; x < 120; x++) gray[y * 480 + x] = 0;
        }
        gray[50 * 480 + 200] = 0; // one noisy pixel must not keep this row
        int[] out = Preprocess.compressGrayGaps(gray, 128, 8);
        assertEquals(0, out[8 * 480 + 100], "first band after capped leading gap");
        assertEquals(0, out[26 * 480 + 100], "second band after capped middle gap");
        assertEquals(255, out[18 * 480 + 100], "middle gap retained as white");
    }

    private static byte[] makePageBytes(Random rnd) {
        byte[] d = new byte[48000];
        Arrays.fill(d, (byte) 0xFF);
        for (int i = 0; i < 2000; i++) {
            int off = rnd.nextInt(48000);
            int len = 1 + rnd.nextInt(30);
            byte v = (byte) rnd.nextInt(256);
            for (int j = 0; j < len && off + j < 48000; j++) d[off + j] = v;
        }
        return d;
    }

    private static int leInt(byte[] h, int off) {
        return (h[off] & 0xFF) | ((h[off + 1] & 0xFF) << 8)
                | ((h[off + 2] & 0xFF) << 16) | ((h[off + 3] & 0xFF) << 24);
    }

    private static void assertTrue(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    private static void assertEquals(Object expected, Object actual, String msg) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(msg + ": expected " + expected + " got " + actual);
        }
    }
}
