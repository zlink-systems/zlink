package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneEntrySpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class EntryZoneJoinHandler
    implements ZLinkEntrySpotActorRequestHandler<
        ZoneEntrySpot,
        PlayerActor,
        Messages.JoinWorldReq,
        Messages.JoinWorldRes> {
    @Override
    public CompletionStage<Messages.JoinWorldRes> handle(
        ZoneEntrySpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.JoinWorldReq request) {
        if (!actor.actorId().equals(request.playerId())) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException("Join player does not match the actor"));
        }
        String zone = ZoneWorldSpec.zoneOf(actor.x(), actor.y());
        actor.context().joinSpot(
            zone,
            new Messages.EnterZoneMsg(
                actor.actorId(), actor.x(), actor.y(), false, true, ""))
            .timeout(Duration.ofSeconds(10))
            .defer();
        return CompletableFuture.completedFuture(
            new Messages.JoinWorldRes(
                actor.actorId(), zone, actor.x(), actor.y(), null));
    }
}
