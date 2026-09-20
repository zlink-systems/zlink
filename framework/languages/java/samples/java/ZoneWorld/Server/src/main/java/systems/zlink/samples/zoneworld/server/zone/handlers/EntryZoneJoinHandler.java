package systems.zlink.samples.zoneworld.server.zone.handlers;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneEntrySpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class EntryZoneJoinHandler
        implements ZLinkEntrySpotActorSendHandler<
                ZoneEntrySpot, PlayerActor, Messages.JoinWorldMsg> {
    @Override
    public CompletionStage<Void> handle(
            ZoneEntrySpot spot,
            PlayerActor actor,
            ZLinkMessageContext context,
            Messages.JoinWorldMsg request) {
        if (!actor.actorId().equals(request.playerId())) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("Join player does not match the actor"));
        }
        // --8<-- [start:doc-zw-entry-join]
        actor.prepareEntry(ZoneWorldSpec.SPAWN_X, ZoneWorldSpec.SPAWN_Y, false, 0, 0);
        String zone = ZoneWorldSpec.zoneOf(ZoneWorldSpec.SPAWN_X, ZoneWorldSpec.SPAWN_Y);
        actor.context()
                .joinSpot(
                        zone,
                        new Messages.EnterZoneReq(
                                actor.actorId(),
                                ZoneWorldSpec.SPAWN_X,
                                ZoneWorldSpec.SPAWN_Y,
                                false,
                                true,
                                "",
                                false))
                .timeout(Duration.ofSeconds(10))
                .defer();
        // --8<-- [end:doc-zw-entry-join]
        return CompletableFuture.completedFuture(null);
    }
}
