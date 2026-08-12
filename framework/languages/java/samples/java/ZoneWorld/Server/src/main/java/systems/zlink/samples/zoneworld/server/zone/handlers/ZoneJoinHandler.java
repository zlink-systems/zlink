package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class ZoneJoinHandler {
    @ZLinkSpotActorRequest
    public CompletionStage<Messages.JoinWorldRes> handle(
        ZoneSpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.JoinWorldReq request) {
        if (actor.pendingJoin()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("Zone actor is not ready"));
        }
        return CompletableFuture.completedFuture(
            new Messages.JoinWorldRes(
                actor.actorId(), actor.zoneId(), actor.x(), actor.y(), null));
    }
}
