package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class BindActorsReqHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.BindActorsReq> {
    private final ZLinkRouteClient routes;

    public BindActorsReqHandler(ZLinkRouteClient routes) {
        this.routes = routes;
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
                Contracts.SPOT_MESH,
                RoutingId.from("play-a"),
                request)
            .timeout(Duration.ofSeconds(30))
            .submit(Contracts.BindActorsRes.class)
            .thenCompose(reply -> bindAll(context, reply)
                .thenRun(() -> context.client().reply(reply).submit()));
    }

    private static CompletionStage<Void> bindAll(
        ZLinkSessionContext context,
        Contracts.BindActorsRes reply) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (Contracts.ActorBinding actor : reply.actors()) {
            chain = chain.thenCompose(ignored -> context.actors().bind(new ActorRef(
                    RoutingId.from(actor.nodeRid()),
                    actor.actorId(),
                    actor.generation()))
                .thenApply(bound -> null));
        }
        return chain;
    }
}
