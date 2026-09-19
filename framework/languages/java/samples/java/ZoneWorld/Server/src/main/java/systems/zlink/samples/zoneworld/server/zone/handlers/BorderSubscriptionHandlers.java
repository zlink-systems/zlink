package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class BorderSubscriptionHandlers {
    private BorderSubscriptionHandlers() {
    }

    // --8<-- [start:doc-zw-border-subscribe]
    public static Class<?> forRoute(String fromZoneId, String toZoneId) {
        return switch (ZoneWorldNames.borderTopic(fromZoneId, toZoneId)) {
            case ZoneWorldNames.NW_NE -> NorthWestToNorthEast.class;
            case ZoneWorldNames.NW_SW -> NorthWestToSouthWest.class;
            case ZoneWorldNames.NE_NW -> NorthEastToNorthWest.class;
            case ZoneWorldNames.NE_SE -> NorthEastToSouthEast.class;
            case ZoneWorldNames.SW_NW -> SouthWestToNorthWest.class;
            case ZoneWorldNames.SW_SE -> SouthWestToSouthEast.class;
            case ZoneWorldNames.SE_NE -> SouthEastToNorthEast.class;
            case ZoneWorldNames.SE_SW -> SouthEastToSouthWest.class;
            default -> throw new IllegalArgumentException("unknown border route: "
                + fromZoneId + " -> " + toZoneId);
        };
    }
    // --8<-- [end:doc-zw-border-subscribe]

    private static CompletionStage<Void> apply(ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_NE)
    public static final class NorthWestToNorthEast {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_SW)
    public static final class NorthWestToSouthWest {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_NW)
    public static final class NorthEastToNorthWest {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_SE)
    public static final class NorthEastToSouthEast {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_NW)
    public static final class SouthWestToNorthWest {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_SE)
    public static final class SouthWestToSouthEast {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_NE)
    public static final class SouthEastToNorthEast {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_SW)
    public static final class SouthEastToSouthWest {
        public CompletionStage<Void> handle(ZoneSpot spot, Messages.ZoneBorderEvent event) { return apply(spot, event); }
    }
}
