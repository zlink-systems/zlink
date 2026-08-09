package systems.zlink.samples.zoneworld.server.zone.spots;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class ZoneEntrySpot implements ZLinkEntrySpot<PlayerActor> {
    private final ZLinkEntrySpotContext context;

    public ZoneEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        PlayerActor actor,
        ZLinkMessage createRequest) {
        Messages.EnterWorldReq request = createRequest.decode(Messages.EnterWorldReq.class);
        if (!ZoneWorldSpec.inRange(request.x(), request.y())) {
            return CompletableFuture.completedFuture(
                ZLinkActorCreateResponse.reject(
                    new Messages.EnterWorldRes("", request.x(), request.y(), "OutOfRange")));
        }
        String zone = ZoneWorldSpec.zoneOf(request.x(), request.y());
        actor.prepareEntry(request.x(), request.y(), request.isBot(), request.dirX(), request.dirY());
        if (request.isBot()) {
            actor.context().joinSpot(
                zone,
                new Messages.EnterZoneMsg(
                    actor.actorId(),
                    request.x(),
                    request.y(),
                    true,
                    true,
                    ""))
                .timeout(Duration.ofSeconds(10))
                .defer();
        }
        return CompletableFuture.completedFuture(
            ZLinkActorCreateResponse.accept(
                new Messages.JoinWorldRes(
                    actor.actorId(), zone, request.x(), request.y(), null)));
    }

    @Override
    public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(PlayerActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
