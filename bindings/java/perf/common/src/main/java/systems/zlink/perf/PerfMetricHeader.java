/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;
final class PerfMetricHeader {
    private static final int GENERIC_MAGIC = 0x5A4C4E4B; // ZLNK

    private PerfMetricHeader() {
    }

    static PerfUtil.Header decode(Message message, int expectedSize) {
        return decode(message, expectedSize, PerfUtil.nowNs());
    }

    static PerfUtil.Header decode(Message message, int expectedSize,
                                  long receivedNanoTime) {
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return null;
        }
        if (message.readIntLe(0) != GENERIC_MAGIC) {
            return null;
        }
        if (message.readIntLe(4) != PerfMeasurement.runId()) {
            return null;
        }
        int phase = message.readIntLe(8) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return null;
        }
        if (message.readIntLe(9) != expectedSize) {
            return null;
        }
        long sentTsNs = message.readLongLe(21);
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
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return false;
        }
        if (message.readIntLe(0) != GENERIC_MAGIC) {
            return false;
        }
        if (message.readIntLe(4) != PerfMeasurement.runId()) {
            return false;
        }
        int phase = message.readIntLe(8) & 0xFF;
        if (phase != PerfUtil.PHASE_ACTIVE) {
            return false;
        }
        if (message.readIntLe(9) != expectedSize) {
            return false;
        }
        long sentTsNs = message.readLongLe(21);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }

    static int phase(Message message, int expectedSize) {
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (message.readIntLe(0) != GENERIC_MAGIC
            || message.readIntLe(4) != PerfMeasurement.runId()
            || message.readIntLe(9) != expectedSize) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = message.readIntLe(8) & 0xFF;
        return phase == PerfUtil.PHASE_WARMUP
            || phase == PerfUtil.PHASE_ACTIVE
            || phase == PerfUtil.PHASE_COOLDOWN
            ? phase : PerfUtil.PHASE_UNKNOWN;
    }

    static int recordOneWayLatency(PerfUtil.Metrics metrics, Message message,
                                   int expectedSize, long activeEnd) {
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (message.readIntLe(0) != GENERIC_MAGIC
            || message.readIntLe(4) != PerfMeasurement.runId()
            || message.readIntLe(9) != expectedSize) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = message.readIntLe(8) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (phase == PerfUtil.PHASE_ACTIVE) {
            long receivedAt = PerfUtil.nowNs();
            if (receivedAt < activeEnd) {
                long sentTsNs = message.readLongLe(21);
                metrics.recordNanos(Math.max(0L, receivedAt - sentTsNs));
            }
        }
        return phase;
    }
}
