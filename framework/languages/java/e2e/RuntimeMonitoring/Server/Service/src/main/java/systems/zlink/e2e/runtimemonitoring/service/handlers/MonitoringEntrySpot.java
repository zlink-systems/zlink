package systems.zlink.e2e.runtimemonitoring.service.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

public final class MonitoringEntrySpot implements ZLinkEntrySpot<MonitoringActor> {
    private final ZLinkEntrySpotContext context;

    public MonitoringEntrySpot(ZLinkEntrySpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(DestroyHandler.class);
    }

    @Override
    public CompletionStage<Void> onJoinedActor(MonitoringActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(MonitoringActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    public static final class DestroyHandler implements ZLinkEntrySpotActorRequestHandler<
        MonitoringEntrySpot,
        MonitoringActor,
        Contracts.PlacementActorDestroyReq,
        Contracts.PlacementActorDestroyRes> {
        @Override
        public CompletionStage<Contracts.PlacementActorDestroyRes> handle(
            MonitoringEntrySpot entrySpot,
            MonitoringActor actor,
            ZLinkMessageContext messageContext,
            Contracts.PlacementActorDestroyReq request) {
            if (!actor.context().actorId().equals(request.actorId())) {
                return CompletableFuture.failedFuture(
                    new IllegalArgumentException("actor id does not match request"));
            }
            return entrySpot.context().destroyActor(actor).thenApply(
                ignored -> new Contracts.PlacementActorDestroyRes(
                    request.actorId(), true));
        }
    }
}
