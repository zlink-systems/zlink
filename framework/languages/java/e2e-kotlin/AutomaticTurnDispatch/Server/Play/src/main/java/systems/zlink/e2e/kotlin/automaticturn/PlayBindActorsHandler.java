package systems.zlink.e2e.kotlin.automaticturn;

import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteRequestContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class PlayBindActorsHandler
    implements ZLinkRouteRequestHandler<Contracts.BindActorsReq, Contracts.BindActorsRes> {
    private final ZLinkActorManager actors;
    private final ZLinkSpotManager spots;
    private final PlayEvidenceStore evidence;

    public PlayBindActorsHandler(
        ZLinkActorManager actors,
        ZLinkSpotManager spots,
        PlayEvidenceStore evidence) {
        this.actors = actors;
        this.spots = spots;
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.BindActorsRes> handle(
        Contracts.BindActorsReq request,
        ZLinkRouteRequestContext context) {
        return spots.getOrCreate(
                ProbeSpot.class,
                RoutingId.from(request.spotRid()),
                ZLinkMessage.of("bind"))
            .thenCompose(ignored -> bind(request.spotRid(), request.actorA())
                .thenCombine(
                    bind(request.spotRid(), request.actorB()),
                    (actorA, actorB) -> new Contracts.BindActorsRes(
                        request.spotRid(),
                        request.actorA(),
                        request.actorB(),
                        List.of(binding(actorA), binding(actorB)))));
    }

    private CompletionStage<ActorRef> bind(String spotRid, String actorId) {
        return actors.getOrCreate(actorId, "probe")
            .thenApply(actor -> {
                evidence.record("bind-actors", "bind-actor", "spot=" + spotRid
                    + ";actor=" + actor.actorId()
                    + ";node=" + actor.nodeRid());
                return actor;
            });
    }

    private static Contracts.ActorBinding binding(ActorRef actor) {
        return new Contracts.ActorBinding(
            actor.actorId(),
            actor.nodeRid().toString(),
            actor.generation());
    }
}
