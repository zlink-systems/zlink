/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.internal.ContractAccess;
final class PerfMetricHeader {
    private static final int GENERIC_MAGIC = 0x5A4C4E4B; // ZLNK

    private PerfMetricHeader() {
    }

    static PerfUtil.Header decode(Message message, int expectedSize) {
        return decode(message, expectedSize, PerfUtil.nowNs());
    }

    static PerfUtil.Header decode(Message message, int expectedSize,
                                  long receivedNanoTime) {
        int phase = headerPhase(message, expectedSize);
        if (phase == PerfUtil.PHASE_UNKNOWN) {
            return null;
        }
        long sentTsNs = ContractAccess.messageMetricHeaderSentTimestamp(message);
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
        int phase = headerPhase(message, expectedSize);
        if (phase != PerfUtil.PHASE_ACTIVE) {
            return false;
        }
        long sentTsNs = ContractAccess.messageMetricHeaderSentTimestamp(message);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }

    static int phase(Message message, int expectedSize) {
        return headerPhase(message, expectedSize);
    }

    static int recordOneWayLatency(PerfUtil.Metrics metrics, Message message,
                                   int expectedSize, long activeEnd) {
        int phase = headerPhase(message, expectedSize);
        if (phase == PerfUtil.PHASE_UNKNOWN) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (phase == PerfUtil.PHASE_ACTIVE) {
            long receivedAt = PerfUtil.nowNs();
            if (receivedAt < activeEnd) {
                long sentTsNs = ContractAccess.messageMetricHeaderSentTimestamp(
                    message);
                metrics.recordNanos(Math.max(0L, receivedAt - sentTsNs));
            }
        }
        return phase;
    }

    private static int headerPhase(Message message, int expectedSize) {
        if (message == null) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = ContractAccess.messageMetricHeaderPhase(message,
            expectedSize, PerfMeasurement.runId());
        return phase < 0 ? PerfUtil.PHASE_UNKNOWN : phase;
    }
}
