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
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return null;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 0)
            != GENERIC_MAGIC) {
            return null;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 4)
            != PerfMeasurement.runId()) {
            return null;
        }
        int phase = ContractAccess.messageReadIntLeUnchecked(message, 8) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return null;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 9)
            != expectedSize) {
            return null;
        }
        long sentTsNs = ContractAccess.messageReadLongLeUnchecked(message, 21);
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
        if (ContractAccess.messageReadIntLeUnchecked(message, 0)
            != GENERIC_MAGIC) {
            return false;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 4)
            != PerfMeasurement.runId()) {
            return false;
        }
        int phase = ContractAccess.messageReadIntLeUnchecked(message, 8) & 0xFF;
        if (phase != PerfUtil.PHASE_ACTIVE) {
            return false;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 9)
            != expectedSize) {
            return false;
        }
        long sentTsNs = ContractAccess.messageReadLongLeUnchecked(message, 21);
        long latencyNanos = Math.max(0L, receivedNanoTime - sentTsNs);
        metrics.recordNanos(halfRoundTrip ? latencyNanos / 2L : latencyNanos);
        return true;
    }

    static int phase(Message message, int expectedSize) {
        if (message == null || message.size() < PerfUtil.HEADER_SIZE) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (ContractAccess.messageReadIntLeUnchecked(message, 0)
                != GENERIC_MAGIC
            || ContractAccess.messageReadIntLeUnchecked(message, 4)
                != PerfMeasurement.runId()
            || ContractAccess.messageReadIntLeUnchecked(message, 9)
                != expectedSize) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = ContractAccess.messageReadIntLeUnchecked(message, 8) & 0xFF;
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
        if (ContractAccess.messageReadIntLeUnchecked(message, 0)
                != GENERIC_MAGIC
            || ContractAccess.messageReadIntLeUnchecked(message, 4)
                != PerfMeasurement.runId()
            || ContractAccess.messageReadIntLeUnchecked(message, 9)
                != expectedSize) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        int phase = ContractAccess.messageReadIntLeUnchecked(message, 8) & 0xFF;
        if (phase != PerfUtil.PHASE_WARMUP
            && phase != PerfUtil.PHASE_ACTIVE
            && phase != PerfUtil.PHASE_COOLDOWN) {
            return PerfUtil.PHASE_UNKNOWN;
        }
        if (phase == PerfUtil.PHASE_ACTIVE) {
            long receivedAt = PerfUtil.nowNs();
            if (receivedAt < activeEnd) {
                long sentTsNs = ContractAccess.messageReadLongLeUnchecked(
                    message, 21);
                metrics.recordNanos(Math.max(0L, receivedAt - sentTsNs));
            }
        }
        return phase;
    }
}
