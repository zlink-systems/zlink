/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class PerfMetricHeader {
    private static final int GENERIC_MAGIC = 0x5A4C4E4B; // ZLNK
    private static final int HEADER_SIZE = 29;
    private static final int RUN_ID_OFFSET = 4;
    private static final int PHASE_OFFSET = 8;
    private static final int MESSAGE_SIZE_OFFSET = 9;
    private static final int SENT_TIMESTAMP_OFFSET = 21;

    private PerfMetricHeader() {
    }

    static PerfUtil.Header decode(Message message, int expectedSize) {
        return decode(message, expectedSize, PerfUtil.nowNs());
    }

    static PerfUtil.Header decode(Message message, int expectedSize,
                                  long receivedNanoTime) {
        ByteBuffer header = header(message, expectedSize);
        if (header == null) {
            return null;
        }
        int phase = header.get(PHASE_OFFSET) & 0xFF;
        long sentTsNs = header.getLong(SENT_TIMESTAMP_OFFSET);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        return new PerfUtil.Header((byte) phase, latencyNanos, sentTsNs);
    }

    static boolean recordActiveLatency(PerfUtil.Metrics metrics,
                                       Message message,
                                       int expectedSize,
                                       boolean halfRoundTrip) {
        return recordActiveLatency(metrics, message, expectedSize,
            halfRoundTrip, PerfUtil.nowNs());
    }

    static boolean recordActiveLatency(PerfUtil.Metrics metrics,
                                       Message message,
                                       int expectedSize,
                                       boolean halfRoundTrip,
                                       long receivedNanoTime) {
        ByteBuffer header = header(message, expectedSize);
        if (header == null || (header.get(PHASE_OFFSET) & 0xFF)
            != PerfUtil.PHASE_ACTIVE) {
            return false;
        }
        long sentTsNs = header.getLong(SENT_TIMESTAMP_OFFSET);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }

    static int phase(Message message, int expectedSize) {
        return headerPhase(message, expectedSize);
    }

    static int recordOneWayLatency(PerfUtil.Metrics metrics, Message message,
                                   int expectedSize, long activeEnd) {
        ByteBuffer header = header(message, expectedSize);
        if (header == null) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = header.get(PHASE_OFFSET) & 0xFF;
        if (phase == PerfUtil.PHASE_ACTIVE) {
            long receivedAt = PerfUtil.nowNs();
            if (receivedAt < activeEnd) {
                long sentTsNs = header.getLong(SENT_TIMESTAMP_OFFSET);
                metrics.recordNanos(Math.max(0L, receivedAt - sentTsNs));
            }
        }
        return phase;
    }

    private static int headerPhase(Message message, int expectedSize) {
        ByteBuffer header = header(message, expectedSize);
        if (header == null) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        return header.get(PHASE_OFFSET) & 0xFF;
    }

    private static ByteBuffer header(Message message, int expectedSize) {
        if (message == null) {
            return null;
        }
        ByteBuffer data = message.dataBuffer().duplicate()
            .order(ByteOrder.LITTLE_ENDIAN);
        if (data.remaining() < HEADER_SIZE
            || data.getInt(0) != GENERIC_MAGIC
            || data.getInt(RUN_ID_OFFSET) != PerfMeasurement.runId()
            || data.getInt(MESSAGE_SIZE_OFFSET) != expectedSize) {
            return null;
        }
        int phase = data.get(PHASE_OFFSET) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return null;
        }
        return data;
    }
}
