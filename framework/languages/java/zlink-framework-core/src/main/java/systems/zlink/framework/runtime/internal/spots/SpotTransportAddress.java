package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.spots.ZLinkSpotKind;

public record SpotTransportAddress(
        String routerChannelId,
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        ZLinkSpotKind spotKind,
        String stableType,
        String ownerId,
        String storeVersion) {
    public static SpotTransportAddress fromRoute(
            systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers.SpotRoute route) {
        return new SpotTransportAddress(
                route.meshName(),
                route.nodeRid(),
                route.spotId(),
                route.spotGeneration(),
                route.targetNodeGeneration(),
                route.authorityOwnerGeneration(),
                route.ownerLeaseGeneration(),
                route.spotKind(),
                route.stableType(),
                route.ownerId(),
                route.storeVersion());
    }

    public SpotTransportAddress(
            String routerChannelId,
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            long targetNodeGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration,
            ZLinkSpotKind spotKind) {
        this(
                routerChannelId,
                targetNodeRid,
                spotId,
                spotGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                spotKind,
                "",
                "",
                "");
    }

    public SpotTransportAddress(
            String routerChannelId,
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            ZLinkSpotKind spotKind) {
        this(routerChannelId, targetNodeRid, spotId, spotGeneration, 0L, 0L, 0L, spotKind);
    }

    public SpotTransportAddress(
            String routerChannelId,
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            long authorityOwnerGeneration,
            ZLinkSpotKind spotKind) {
        this(
                routerChannelId,
                targetNodeRid,
                spotId,
                spotGeneration,
                0L,
                authorityOwnerGeneration,
                0L,
                spotKind);
    }
}
