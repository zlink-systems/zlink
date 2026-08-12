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
    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_NE)
    public CompletionStage<Void> northWestToNorthEast(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NW_SW)
    public CompletionStage<Void> northWestToSouthWest(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_NW)
    public CompletionStage<Void> northEastToNorthWest(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.NE_SE)
    public CompletionStage<Void> northEastToSouthEast(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_NW)
    public CompletionStage<Void> southWestToNorthWest(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SW_SE)
    public CompletionStage<Void> southWestToSouthEast(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_NE)
    public CompletionStage<Void> southEastToNorthEast(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = ZoneWorldNames.SE_SW)
    public CompletionStage<Void> southEastToSouthWest(
        ZoneSpot spot, Messages.ZoneBorderEvent event) {
        spot.applyBorder(event);
        return CompletableFuture.completedFuture(null);
    }
}
