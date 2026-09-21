package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;

/** Identifies one logical creation operation within an exact source-node lifecycle. */
public record ZLinkCreationOperationIdentity(
        RoutingId sourceNodeRid,
        long sourceLifecycleGeneration,
        long operationIdHigh,
        long operationIdLow) {
    public ZLinkCreationOperationIdentity {
        Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        if (sourceLifecycleGeneration == 0) {
            throw new IllegalArgumentException("sourceLifecycleGeneration must be positive");
        }
        if (operationIdHigh == 0 && operationIdLow == 0) {
            throw new IllegalArgumentException("operationId must not be zero");
        }
    }
}
