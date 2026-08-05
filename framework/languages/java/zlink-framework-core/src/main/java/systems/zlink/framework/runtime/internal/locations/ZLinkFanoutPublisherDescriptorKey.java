package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

public record ZLinkFanoutPublisherDescriptorKey(
    String channelName,
    RoutingId publisherRid) {
}
