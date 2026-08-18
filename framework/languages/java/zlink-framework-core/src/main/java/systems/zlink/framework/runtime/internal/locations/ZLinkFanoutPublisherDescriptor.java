package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.transport.ZLinkEndpointNotation;

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
    public ZLinkFanoutPublisherDescriptor {
        //  Write-time normalization (endpoint notation policy §2.3): see
        //  ZLinkMeshNodeDescriptor. Endpoint blank/null validation stays
        //  external (ZLinkInMemoryLocationStore.validateFanoutDescriptor)
        //  since this record never validated on its own; normalize() is a
        //  no-op passthrough for null/blank input.
        endpoint = ZLinkEndpointNotation.normalize(endpoint);
    }
}
