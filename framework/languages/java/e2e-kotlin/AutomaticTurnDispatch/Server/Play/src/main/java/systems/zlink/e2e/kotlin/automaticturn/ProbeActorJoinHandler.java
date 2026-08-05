package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.spots.ZLinkSpotActorRequestContext;

public final class ProbeActorJoinHandler {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ProbeSpot spot,
        ProbeActor actor,
        ZLinkSpotActorRequestContext context,
        Contracts.ActorJoinReq request) {
        CompletionStage<?> delay = request.millis() > 0
            ? spot.context().outbound()
                .requestToChannel(Contracts.DELAY_CHANNEL, new Contracts.DelayReq(request.value(), request.millis()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
            : CompletableFuture.completedFuture(null);
        return delay.thenCompose(ignored -> actor.context()
                .joinSpot(RoutingId.from(request.spotRid()), request)
                .submit(Contracts.ActorJoinRes.class))
            .thenApply(joined -> new Contracts.ActorJoinRes(
                actor.actorId(),
                request.spotRid(),
                "joined:" + request.value()));
    }
}
