/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Bench spec section 6: a 29-byte measurement header at the front of the payload body.
 * The layout is identical in every language; a language that invents its own
 * layout cannot have its cells put next to another language's.
 *
 * <pre>
 *   0  4  magic 0x5A4C4E4B ("ZLNK")
 *   4  4  run id
 *   8  1  phase (0 warmup, 1 active)
 *   9  4  payload size
 *  13  8  sequence
 *  21  8  send timestamp ns
 * </pre>
 */
public final class BenchMetricHeader {
    public static final int HEADER_SIZE = 29;
    public static final int MAGIC = 0x5a4c4e4b;
    public static final byte PHASE_WARMUP = 0;
    public static final byte PHASE_ACTIVE = 1;

    private BenchMetricHeader() {
    }

    /**
     * Monotonic nanoseconds. {@code System.nanoTime()} is CLOCK_MONOTONIC on this
     * host, so the client and the server read the same timeline -- which is what
     * makes the server-side receive latency of {@code send-saturation} meaningful.
     * Wall clock is not used: this host's wall clock jumps by seconds.
     */
    public static long nowNs() {
        return System.nanoTime();
    }

    public static byte[] createPayload(int payloadSize, int runId, byte phase, long sequence) {
        if (payloadSize < HEADER_SIZE) {
            throw new IllegalArgumentException("payload size must be at least " + HEADER_SIZE);
        }
        byte[] bytes = new byte[payloadSize];
        java.util.Arrays.fill(bytes, (byte) 0xab);
        stamp(bytes, runId, phase, payloadSize, sequence);
        return bytes;
    }

    public static void stamp(byte[] bytes, int runId, byte phase, int payloadSize, long sequence) {
        ByteBuffer buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(0, MAGIC);
        buffer.putInt(4, runId);
        buffer.put(8, phase);
        buffer.putInt(9, payloadSize);
        buffer.putLong(13, sequence);
        buffer.putLong(21, nowNs());
    }

    /** Decoded header, or {@code null} when the bytes are not a bench header. */
    public static Decoded decode(ByteBuffer body) {
        if (body == null || body.remaining() < HEADER_SIZE) {
            return null;
        }
        ByteBuffer view = body.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        int base = view.position();
        if (view.getInt(base) != MAGIC) {
            return null;
        }
        return new Decoded(
            view.getInt(base + 4),
            view.get(base + 8),
            view.getInt(base + 9),
            view.getLong(base + 13),
            view.getLong(base + 21));
    }

    public static Decoded decode(byte[] body) {
        return body == null ? null : decode(ByteBuffer.wrap(body));
    }

    /** G2: the request patterns validate the header that came back. */
    public static boolean isExpected(
        Decoded header, int runId, byte phase, int payloadSize, long sequence) {
        return header != null
            && header.runId == runId
            && header.phase == phase
            && header.payloadSize == payloadSize
            && header.sequence == sequence;
    }

    public record Decoded(int runId, byte phase, int payloadSize, long sequence,
                          long sentTimestampNs) {
    }
}
