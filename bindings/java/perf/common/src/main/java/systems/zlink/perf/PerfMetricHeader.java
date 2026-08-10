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
        long latencyNanos = Math.max(0L, PerfUtil.nowNs() - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }
}
