package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/** Immutable root pointer stored with a committed aggregate. */
public record ZLinkAggregateProgress(
    String reference,
    long checksumCrc32c) {

    public ZLinkAggregateProgress {
        if (reference == null || reference.isBlank()) {
            throw new IllegalArgumentException(
                "aggregate progress reference must be non-blank");
        }
    }
}
