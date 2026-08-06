package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.ZLinkMessageContext;

public final class ProbeActorJoinHandler {
    @ZLinkSpotActorRequest(packetName = "ActorJoinReq")
    public CompletionStage<Contracts.ActorJoinRes> handle(
        ProbeSpot spot,
        ProbeActor actor,
        ZLinkMessageContext context,
        Contracts.ActorJoinReq request) {
        CompletionStage<?> delay = request.millis() > 0
            ? spot.context().outbound()
                .requestToChannel(Contracts.DELAY_CHANNEL, new Contracts.DelayReq(request.value(), request.millis()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DelayRes.class)
            : CompletableFuture.completedFuture(null);
        return delay.thenApply(ignored -> {
            actor.context()
                .joinSpot(request.spotRid(), request)
                .defer();
            return new Contracts.ActorJoinRes(
                actor.context().actorId(),
                request.spotRid(),
                "join-deferred:" + request.value());
        });
    }
}
