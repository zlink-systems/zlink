package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/**
 * Framework-private progress stored in the committed aggregate marker.
 * Participant authority rows retain their application authority and owner
 * fence; this record is the single mutable root pointer for the aggregate.
 */
public record ZLinkAggregateProgress(
    String reference,
    long checksumCrc32c,
    int phase,
    boolean sourceCleanupCompleted,
    int terminalCompletionCount,
    int pendingRelayCount) {
    public ZLinkAggregateProgress(
        String reference,
        long checksumCrc32c,
        int phase,
        boolean sourceCleanupCompleted) {
        this(reference, checksumCrc32c, phase, sourceCleanupCompleted, 0, 0);
    }

    public ZLinkAggregateProgress {
        if (reference == null || reference.isBlank()) {
            throw new IllegalArgumentException(
                "aggregate progress reference must be non-blank");
        }
        if (phase != 4 && phase != 8) {
            throw new IllegalArgumentException(
                "aggregate progress phase must be 4 or 8");
        }
        if (sourceCleanupCompleted != (phase == 8)) {
            throw new IllegalArgumentException(
                "aggregate progress cleanup state differs from phase");
        }
        if (terminalCompletionCount < 0 || pendingRelayCount < 0
            || pendingRelayCount > terminalCompletionCount) {
            throw new IllegalArgumentException(
                "aggregate completion counters are invalid");
        }
    }
}
