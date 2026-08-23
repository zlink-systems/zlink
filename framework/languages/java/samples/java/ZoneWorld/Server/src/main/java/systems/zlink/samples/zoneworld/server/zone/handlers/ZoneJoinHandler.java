package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class ZoneJoinHandler {
    @ZLinkSpotActorSend
    public CompletionStage<Void> handle(
        ZoneSpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.JoinWorldReq request) {
        if (actor.pendingJoin()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("Zone actor is not ready"));
        }
        return actor.send(new Messages.JoinWorldRes(
            actor.actorId(), actor.zoneId(), actor.x(), actor.y(), null));
    }
}
