package systems.zlink.e2e.observabilityops.play;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.shared.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

public final class ObservabilityActorPushHandler implements
    ZLinkEntrySpotActorRequestHandler<ObservabilityEntrySpot, AwaitActor,
        Contracts.ActorPushAwaitReq, Contracts.ActorPushAwaitRes> {
    private final EvidenceStore evidence;

    public ObservabilityActorPushHandler(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.ActorPushAwaitRes> handle(
        ObservabilityEntrySpot spot,
        AwaitActor actor,
        ZLinkMessageContext context,
        Contracts.ActorPushAwaitReq request) {
        String value = "spot=" + spot.context().spotId()
            + ";actor=" + actor.actorId() + ";handler=actor";
        evidence.record("actor-push-await-started", request.requestId(), value);
        evidence.record("actor-push-await-released", request.requestId(), value);

        CompletableFuture<Void> delay = new CompletableFuture<>();
        long delayMillis = Math.max(0L, request.delayMillis());
        CompletableFuture.delayedExecutor(delayMillis, TimeUnit.MILLISECONDS)
            .execute(() -> delay.complete(null));
        return delay.thenCompose(ignored -> {
            evidence.record("actor-push-await-resumed", request.requestId(), value);
            return actor.context().boundSession().send(new Contracts.ActorPushNotify(
                actor.actorId(), request.requestId(), request.value(),
                spot.context().nodeRid().toString())).submit();
        }).thenApply(ignored -> {
            evidence.record("actor-push-await-completed", request.requestId(), value);
            return new Contracts.ActorPushAwaitRes(
                "OBS-C", request.requestId(), actor.actorId(),
                "actor-push-await-completed");
        });
    }
}
