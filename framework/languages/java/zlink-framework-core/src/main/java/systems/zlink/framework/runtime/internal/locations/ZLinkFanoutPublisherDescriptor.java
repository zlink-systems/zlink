package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

public record ZLinkFanoutPublisherDescriptor(
    String channelName,
    RoutingId publisherRid,
    long lifecycleGeneration,
    long descriptorRevision,
    String endpoint,
    ZLinkFrameworkRuntimeState state,
    String securityIdentity,
    String ownerId,
    long leaseGeneration,
    Instant updatedAt) {
}
