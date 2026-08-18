package com.murphy.m4screenbridge.browser.stream;

import java.util.ArrayList;
import java.util.List;

/**
 * Incremental M4B3 envelope assembler for a TCP byte stream.
 * Rejects oversized/overflowing lengths before waiting for a large body.
 */
public final class M4B3Framer {
    private final byte[] buf = new byte[M4B3.MAX_MESSAGE_SIZE];
    private int filled;

    public List<byte[]> feed(byte[] data, int offset, int length) {
        if (data == null) throw M4B3Exception.truncated("framer data is null");
        if (offset < 0 || length < 0 || offset > data.length || length > data.length - offset) {
            throw M4B3Exception.truncated("framer slice out of bounds");
        }
        List<byte[]> out = new ArrayList<>();
        int pos = offset;
        int remain = length;
        while (remain > 0) {
            int room = buf.length - filled;
            if (room <= 0) throw M4B3Exception.overflow("framer buffer full");
            int n = Math.min(room, remain);
            System.arraycopy(data, pos, buf, filled, n);
            filled += n;
            pos += n;
            remain -= n;
            drain(out);
        }
        return out;
    }

    public List<byte[]> feed(byte[] data) {
        return feed(data, 0, data == null ? 0 : data.length);
    }

    public void reset() {
        filled = 0;
    }

    public int buffered() {
        return filled;
    }

    private void drain(List<byte[]> out) {
        while (filled >= M4B3.ENVELOPE_SIZE) {
            if (buf[0] != M4B3.MAGIC[0] || buf[1] != M4B3.MAGIC[1]
                    || buf[2] != M4B3.MAGIC[2] || buf[3] != M4B3.MAGIC[3]) {
                reset();
                throw M4B3Exception.invalid("bad magic in stream");
            }
            int headerLen = (buf[6] & 0xFF) | ((buf[7] & 0xFF) << 8);
            long payloadLen = (buf[8] & 0xFFL)
                    | ((buf[9] & 0xFFL) << 8)
                    | ((buf[10] & 0xFFL) << 16)
                    | ((buf[11] & 0xFFL) << 24);
            if (headerLen > M4B3.MAX_HEADER_LEN) {
                reset();
                throw M4B3Exception.oversized("header_len=" + headerLen);
            }
            if (payloadLen > M4B3.MAX_PAYLOAD_LEN) {
                reset();
                throw M4B3Exception.oversized("payload_len=" + payloadLen);
            }
            long need = (long) M4B3.ENVELOPE_SIZE + headerLen + payloadLen;
            if (need > buf.length) {
                reset();
                throw M4B3Exception.overflow("message " + need);
            }
            if (filled < need) return;
            byte[] msg = new byte[(int) need];
            System.arraycopy(buf, 0, msg, 0, (int) need);
            int leftover = filled - (int) need;
            if (leftover > 0) System.arraycopy(buf, (int) need, buf, 0, leftover);
            filled = leftover;
            out.add(msg);
        }
    }
}
