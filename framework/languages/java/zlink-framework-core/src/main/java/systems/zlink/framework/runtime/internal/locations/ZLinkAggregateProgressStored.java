package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/** Result of the single aggregate-marker progress CAS. */
public record ZLinkAggregateProgressStored(
    ZLinkAggregateProgressSnapshot snapshot)
    implements ZLinkAggregateProgressWriteResult {
    public ZLinkAggregateProgressStored {
        Objects.requireNonNull(snapshot, "snapshot");
    }
}
