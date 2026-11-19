package systems.zlink.framework.runtime.internal.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
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
        String storeVersion,
        ZLinkServiceAuthorityPayloadCodec.State authorityState) {
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
                route.storeVersion(),
                route.authorityState());
    }

    public SpotTransportAddress(
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
        this(
                routerChannelId,
                targetNodeRid,
                spotId,
                spotGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration,
                spotKind,
                stableType,
                ownerId,
                storeVersion,
                ZLinkServiceAuthorityPayloadCodec.State.READY);
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
