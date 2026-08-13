/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;

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
        if (!hasValidHeader(message, expectedSize)) {
            return null;
        }
        int phase = message.readByte(PHASE_OFFSET) & 0xFF;
        long sentTsNs = message.readLongLe(SENT_TIMESTAMP_OFFSET);
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
        if (!hasValidHeader(message, expectedSize)
            || (message.readByte(PHASE_OFFSET) & 0xFF)
            != PerfUtil.PHASE_ACTIVE) {
            return false;
        }
        long sentTsNs = message.readLongLe(SENT_TIMESTAMP_OFFSET);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }

    static int phase(Message message, int expectedSize) {
        return headerPhase(message, expectedSize);
    }

    static int recordOneWayLatency(PerfUtil.Metrics metrics, Message message,
                                   int expectedSize, long activeEnd) {
        if (!hasValidHeader(message, expectedSize)) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = message.readByte(PHASE_OFFSET) & 0xFF;
        if (phase == PerfUtil.PHASE_ACTIVE) {
            long receivedAt = PerfUtil.nowNs();
            if (receivedAt < activeEnd) {
                long sentTsNs = message.readLongLe(SENT_TIMESTAMP_OFFSET);
                metrics.recordNanos(Math.max(0L, receivedAt - sentTsNs));
            }
        }
        return phase;
    }

    private static int headerPhase(Message message, int expectedSize) {
        if (!hasValidHeader(message, expectedSize)) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        return message.readByte(PHASE_OFFSET) & 0xFF;
    }

    private static boolean hasValidHeader(Message message, int expectedSize) {
        // Perf must cross the same public Message surface as an application.
        // These primitive reads avoid allocating a ByteBuffer view for every
        // received frame while retaining the public range checks and wire
        // header validation.
        if (message == null || message.size() < HEADER_SIZE
            || message.readIntLe(0) != GENERIC_MAGIC
            || message.readIntLe(RUN_ID_OFFSET) != PerfMeasurement.runId()
            || message.readIntLe(MESSAGE_SIZE_OFFSET) != expectedSize) {
            return false;
        }
        int phase = message.readByte(PHASE_OFFSET) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return false;
        }
        return true;
    }
}
