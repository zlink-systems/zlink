package systems.zlink.e2e.automaticturn.shared;

import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class PlayBindActorsHandler
    implements ZLinkRouteRequestHandler<Contracts.BindActorsReq, Contracts.BindActorsRes> {
    private final ZLinkActorManager actors;
    private final ZLinkSpotManager spots;
    private final EvidenceStore evidence;

    public PlayBindActorsHandler(
        ZLinkActorManager actors,
        ZLinkSpotManager spots,
        EvidenceStore evidence) {
        this.actors = actors;
        this.spots = spots;
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.BindActorsRes> handle(
        Contracts.BindActorsReq request,
        ZLinkRouteMessageContext context) {
        return spots.getOrCreate(
                AwaitProbeSpot.class,
                RoutingId.from(request.spotRid()),
                ZLinkMessage.of("bind"))
            .thenCompose(created -> bind(request.spotRid(), request.actorA()))
            .thenCompose(actorA -> bind(request.spotRid(), request.actorB())
                .thenApply(actorB -> new Contracts.BindActorsRes(
                    request.spotRid(), request.actorA(), request.actorB(),
                    List.of(binding(actorA), binding(actorB)))));
    }

    private CompletionStage<ActorRef> bind(String spotRid, String actorId) {
        return actors.getOrCreate(actorId, Contracts.ACTOR_TYPE)
            .thenApply(actor -> {
                evidence.record("bind-actor", spotRid,
                    "actor=" + actor.actorId() + ";node=" + actor.nodeRid());
                return actor;
            });
    }

    private static Contracts.ActorBinding binding(ActorRef actor) {
        return new Contracts.ActorBinding(
            actor.actorId(),
            actor.nodeRid().toString(),
            actor.objectGeneration());
    }
}
