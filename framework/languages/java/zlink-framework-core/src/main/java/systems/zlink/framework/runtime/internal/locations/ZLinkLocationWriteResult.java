package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;

public record ZLinkLocationWriteResult(
    ZLinkLocationWriteStatus status,
    long generation,
    Instant updatedAt) {

    public static ZLinkLocationWriteResult stored(long generation, Instant updatedAt) {
        return new ZLinkLocationWriteResult(ZLinkLocationWriteStatus.STORED, generation, updatedAt);
    }

    public static ZLinkLocationWriteResult ignoredStale() {
        return new ZLinkLocationWriteResult(ZLinkLocationWriteStatus.IGNORED_STALE, 0, null);
    }

    public static ZLinkLocationWriteResult rejectedConflict() {
        return new ZLinkLocationWriteResult(ZLinkLocationWriteStatus.REJECTED_CONFLICT, 0, null);
    }
}
