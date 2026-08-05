package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class BindActorsHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.BindActorsReq> {
    private final ZLinkRouteClient routes;
    private final EvidenceStore evidence;

    public BindActorsHandler(
        ZLinkRouteClient routes,
        EvidenceStore evidence) {
        this.routes = routes;
        this.evidence = evidence;
    }

    @Override
    public Class<Contracts.BindActorsReq> messageType() {
        return Contracts.BindActorsReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.BindActorsReq request) {
        return routes.requestToNode(
                Contracts.ROUTE_CHANNEL,
                RoutingId.from(Contracts.PLAY_NODE),
                request)
            .timeout(Duration.ofSeconds(30))
            .submit(Contracts.BindActorsRes.class)
            .thenCompose(reply -> bindAll(context, reply)
                .thenRun(() -> {
                    evidence.record("actors-bound", request.spotRid(), request.actorA() + "," + request.actorB());
                    context.client().reply(reply).submit();
                }));
    }

    private static CompletionStage<Void> bindAll(
        ZLinkSessionContext context,
        Contracts.BindActorsRes reply) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (Contracts.ActorBinding actor : reply.actors()) {
            chain = chain.thenCompose(ignored -> context.actors().bind(new ActorRef(
                    RoutingId.from(actor.nodeRid()),
                    actor.actorId(),
                    actor.objectGeneration()))
                .thenApply(bound -> null));
        }
        return chain;
    }
}
