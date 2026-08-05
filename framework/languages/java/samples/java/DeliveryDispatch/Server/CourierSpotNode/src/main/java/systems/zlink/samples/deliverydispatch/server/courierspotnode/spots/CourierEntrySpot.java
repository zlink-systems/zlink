package systems.zlink.samples.deliverydispatch.server.courierspotnode.spots;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.ActorDirectory;
import systems.zlink.samples.deliverydispatch.server.courierspotnode.CourierActor;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class CourierEntrySpot implements ZLinkEntrySpot<CourierActor> {
    private final ZLinkEntrySpotContext context;
    private final ActorDirectory actors;

    public CourierEntrySpot(
        ZLinkEntrySpotContext context,
        ActorDirectory actors) {
        this.context = context;
        this.actors = actors;
    }

    @Override
    public ZLinkEntrySpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
        CourierActor actor,
        ZLinkMessage createRequest) {
        actors.register(actor);
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onJoinedActor(CourierActor actor) {
        actors.register(actor);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(CourierActor actor) {
        actors.remove(actor.actorId());
        return CompletableFuture.completedFuture(null);
    }
}
