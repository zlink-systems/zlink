package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

/**
 * Store-backed discovery identity of one ClientServer server lifecycle.
 */
public record ZLinkClientServerServerDescriptor(
    String channelName,
    RoutingId serverRid,
    long lifecycleGeneration,
    long descriptorRevision,
    String endpoint,
    int weight,
    ZLinkFrameworkRuntimeState state,
    String securityIdentity,
    String ownerId,
    long leaseGeneration,
    Instant updatedAt) {
    public ZLinkClientServerServerDescriptor {
        channelName = requireText(channelName, "channelName");
        Objects.requireNonNull(serverRid, "serverRid");
        endpoint = requireText(endpoint, "endpoint");
        securityIdentity = requireText(
            securityIdentity,
            "securityIdentity");
        ownerId = requireText(ownerId, "ownerId");
        if (lifecycleGeneration <= 0
            || descriptorRevision <= 0
            || leaseGeneration <= 0) {
            throw new IllegalArgumentException(
                "lifecycleGeneration, descriptorRevision and "
                    + "leaseGeneration must be positive");
        }
        if (weight < 0 || weight > 10_000) {
            throw new IllegalArgumentException(
                "weight must be in 0..10000");
        }
        Objects.requireNonNull(state, "state");
        Objects.requireNonNull(updatedAt, "updatedAt");
    }

    private static String requireText(String value, String field) {
        if (value == null
            || value.isBlank()
            || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                field + " must be non-blank text without NUL");
        }
        return value;
    }
}
